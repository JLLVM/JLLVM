// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include "GarbageCollector.hpp"

#include <llvm/ADT/PointerIntPair.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/ErrorHandling.h>

#include <jllvm/class/Descriptors.hpp>
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/object/Object.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#define DEBUG_TYPE "jvm"

static llvm::cl::opt<bool> gcEveryAlloc("jllvm-gc-every-alloc", llvm::cl::Hidden, llvm::cl::init(false));

static constexpr auto STATIC_SLAB_SIZE = 4096 / sizeof(void*);

jllvm::GarbageCollector::GarbageCollector(std::size_t heapSize)
    : m_heapSize(heapSize),
      m_spaceOne(std::make_unique<char[]>(heapSize)),
      m_spaceTwo(std::make_unique<char[]>(heapSize)),
      m_fromSpace(m_spaceOne.get()),
      m_toSpace(m_spaceTwo.get()),
      m_bumpPtr(m_fromSpace),
      m_staticRoots(STATIC_SLAB_SIZE)
{
    m_localRoots.emplace_back(LOCAL_SLAB_SIZE);
    std::memset(m_bumpPtr, 0, m_heapSize);
    __asan_poison_memory_region(m_toSpace, m_heapSize);
}

namespace
{

using PointerIntPair = llvm::PointerIntPair<jllvm::ClassObject*, 1, bool>;

/// Returns a 'PointerIntPair' from the class object field of 'objectInterface', containing the mark bit for the GC.
PointerIntPair classAsPointerIntPair(jllvm::ObjectInterface* objectInterface)
{
    return PointerIntPair::getFromOpaqueValue(
        reinterpret_cast<void*>(const_cast<jllvm::ClassObject*>(objectInterface->getClass())));
}

/// Returns the class object of 'objectInterface'. This should be used during and after the mark phase as accessing
/// the class object normally might not be possible due to mark bit being set in the field.
jllvm::ClassObject* getClass(jllvm::ObjectInterface* objectInterface)
{
    return classAsPointerIntPair(objectInterface).getPointer();
}

/// Returns true if the object has been marked as seen previously.
bool hasBeenSeen(jllvm::ObjectInterface* objectInterface)
{
    return classAsPointerIntPair(objectInterface).getInt();
}

void markSeen(jllvm::ObjectInterface* objectInterface)
{
    auto pair = classAsPointerIntPair(objectInterface);
    pair.setInt(true);
    objectInterface->getObjectHeader().classObject = reinterpret_cast<const jllvm::ClassObject*>(pair.getOpaqueValue());
}

void clearMark(jllvm::ObjectInterface* objectInterface)
{
    auto pair = classAsPointerIntPair(objectInterface);
    pair.setInt(false);
    objectInterface->getObjectHeader().classObject = reinterpret_cast<const jllvm::ClassObject*>(pair.getOpaqueValue());
}

/// Returns the size of 'objectInterface' in number of bytes.
std::size_t getSize(jllvm::ObjectInterface* objectInterface)
{
    std::size_t instanceSize = getClass(objectInterface)->getInstanceSize();
    if (const jllvm::ClassObject* component = getClass(objectInterface)->getComponentType())
    {
        std::size_t length = static_cast<const jllvm::Array<>&>(*objectInterface).size();
        if (component->isPrimitive())
        {
            instanceSize += component->getInstanceSize() * length;
        }
        else
        {
            instanceSize += sizeof(jllvm::Object*) * length;
        }
    }
    return instanceSize;
}

bool shouldBeAddedToWorkList(jllvm::ObjectInterface* repr, jllvm::ObjectInterface* from, jllvm::ObjectInterface* to)
{
    if (!(repr >= from && repr < to))
    {
        return false;
    }
    return !hasBeenSeen(repr);
}

void collectStackRoots(const llvm::DenseMap<std::uintptr_t, std::vector<jllvm::StackMapEntry>>& map,
                       std::vector<jllvm::ObjectInterface*>& results, jllvm::ObjectInterface* from,
                       jllvm::ObjectInterface* to)
{
    llvm::SmallVector<jllvm::ObjectInterface*> buffer;
    jllvm::unwindStack(
        [&](const jllvm::UnwindFrame& context)
        {
            for (const jllvm::StackMapEntry& iter : map.lookup(context.getProgramCounter()))
            {
                // Only the base pointers point to actual objects and are used to mark the object.
                iter.basePointer.readVector(buffer, context);
                for (jllvm::ObjectInterface* object : buffer)
                {
                    if (shouldBeAddedToWorkList(object, from, to))
                    {
                        results.push_back(object);
                        markSeen(object);
                    }
                }
            }
        });
}

void replaceStackRoots(const llvm::DenseMap<std::uintptr_t, std::vector<jllvm::StackMapEntry>>& map,
                       const llvm::DenseMap<jllvm::ObjectInterface*, jllvm::ObjectInterface*>& mapping)
{
    llvm::SmallVector<jllvm::ObjectInterface*> basePointers;
    llvm::SmallVector<std::byte*> derivedPointers;
    jllvm::unwindStack(
        [&](jllvm::UnwindFrame& context)
        {
            for (const jllvm::StackMapEntry& iter : map.lookup(context.getProgramCounter()))
            {
                iter.basePointer.readVector(basePointers, context);
                iter.derivedPointer.readVector(derivedPointers, context);
                for (auto&& [basePointer, derivedPointer] : llvm::zip_equal(basePointers, derivedPointers))
                {
                    if (jllvm::ObjectInterface* replacement = mapping.lookup(basePointer))
                    {
                        // Calculate the original offset of the derived pointer from the base pointer first.
                        std::size_t offset = derivedPointer - reinterpret_cast<std::byte*>(basePointer);
                        // Then reapply it to the replacement.
                        derivedPointer = reinterpret_cast<std::byte*>(replacement) + offset;
                    }
                }
                // Only the derived pointer locations need to be written back to.
                // The base pointers only exist to be able to calculate the offset of the derived.
                iter.derivedPointer.writeVector(derivedPointers, context);
            }
        });
}

template <class F>
void introspectObject(jllvm::ObjectInterface* object, F&& f)
{
    jllvm::ClassObject* classObject = getClass(object);
    if (const jllvm::ClassObject* componentType = classObject->getComponentType())
    {
        // Array of references.
        if (!componentType->isPrimitive())
        {
            auto* array = static_cast<jllvm::Array<jllvm::ObjectInterface*>*>(object);
            for (jllvm::ObjectInterface** iter = array->begin(); iter != array->end(); iter++)
            {
                f(iter);
            }
        }
        return;
    }

    for (std::uint32_t iter : classObject->getGCObjectMask())
    {
        f(reinterpret_cast<jllvm::ObjectInterface**>(reinterpret_cast<char*>(object) + iter * sizeof(jllvm::Object*)));
    }
}

void mark(std::vector<jllvm::ObjectInterface*>& workList, jllvm::ObjectInterface* from, jllvm::ObjectInterface* to)
{
    while (!workList.empty())
    {
        jllvm::ObjectInterface* object = workList.back();
        workList.pop_back();

        introspectObject(object,
                         [&](jllvm::ObjectInterface** field)
                         {
                             jllvm::ObjectInterface* reached = *field;
                             if (shouldBeAddedToWorkList(reached, from, to))
                             {
                                 markSeen(reached);
                                 workList.push_back(reached);
                             }
                         });
    }
}

} // namespace

jllvm::GCRootRef<jllvm::Object> jllvm::GarbageCollector::allocateStatic()
{
    return GCRootRef<Object>(m_staticRoots.allocate());
}

void jllvm::GarbageCollector::garbageCollect()
{
    auto* from = reinterpret_cast<jllvm::ObjectInterface*>(m_fromSpace);
    auto* to = reinterpret_cast<jllvm::ObjectInterface*>(m_bumpPtr);

    std::vector<jllvm::ObjectInterface*> roots;
    collectStackRoots(m_entries, roots, from, to);

    auto addToWorkListLambda = [&roots, from, to](ObjectInterface* object)
    {
        if (shouldBeAddedToWorkList(object, from, to))
        {
            markSeen(object);
            roots.push_back(object);
        }
    };

    llvm::for_each(llvm::make_pointee_range(m_staticRoots), addToWorkListLambda);
    for (RootFreeList& list : m_localRoots)
    {
        llvm::for_each(llvm::make_pointee_range(list), addToWorkListLambda);
    }

    for (RootProvider& provider : llvm::make_pointee_range(m_rootProviders))
    {
        provider.addRootsForRelocation([&](ObjectInterface** interface) { addToWorkListLambda(*interface); });
    }

    mark(roots, from, to);

    auto nextObject = [](char* curr)
    {
        auto* object = reinterpret_cast<jllvm::ObjectInterface*>(curr);
        curr += getSize(object);
        curr += llvm::offsetToAlignedAddr(curr, llvm::Align(alignof(ObjectHeader)));
        return curr;
    };

    [[maybe_unused]] std::size_t collectedObjects = 0;
    [[maybe_unused]] std::size_t relocatedObjects = 0;

    __asan_unpoison_memory_region(m_toSpace, m_heapSize);

    auto* oldBumpPtr = m_bumpPtr;
    m_bumpPtr = m_toSpace;
    std::memset(m_bumpPtr, 0, m_heapSize);
    llvm::DenseMap<jllvm::ObjectInterface*, jllvm::ObjectInterface*> mapping;
    for (char* iter = m_fromSpace; iter != oldBumpPtr; iter = nextObject(iter))
    {
        auto* object = reinterpret_cast<jllvm::ObjectInterface*>(iter);
        auto objectSize = getSize(object);
        if (!hasBeenSeen(object))
        {
            collectedObjects++;
            continue;
        }

        relocatedObjects++;
        clearMark(object);
        auto* newStorage = m_bumpPtr;
        m_bumpPtr += objectSize;
        m_bumpPtr += llvm::offsetToAlignedAddr(m_bumpPtr, llvm::Align(alignof(ObjectHeader)));
        std::memcpy(newStorage, object, objectSize);
        mapping[object] = reinterpret_cast<jllvm::ObjectInterface*>(newStorage);
    }

    LLVM_DEBUG({
        llvm::dbgs() << "GC: Collected " << collectedObjects << " objects, relocated " << relocatedObjects << '\n';
    });

    std::swap(m_fromSpace, m_toSpace);

    __asan_poison_memory_region(m_toSpace, m_heapSize);

    if (mapping.empty())
    {
        return;
    }

    replaceStackRoots(m_entries, mapping);

    auto relocate = [&](ObjectInterface** root)
    {
        if (jllvm::ObjectInterface* replacement = mapping.lookup(*root))
        {
            *root = replacement;
        }
    };

    llvm::for_each(m_staticRoots, relocate);
    for (RootFreeList& list : m_localRoots)
    {
        llvm::for_each(list, relocate);
    }

    for (RootProvider& provider : llvm::make_pointee_range(m_rootProviders))
    {
        provider.addRootsForRelocation([&](ObjectInterface** interface) { relocate(interface); });
    }

    for (char* iter = m_fromSpace; iter != m_bumpPtr; iter = nextObject(iter))
    {
        auto* object = reinterpret_cast<jllvm::ObjectInterface*>(iter);

        introspectObject(object, [&](jllvm::ObjectInterface** field) { relocate(field); });
    }
}

jllvm::GarbageCollector::~GarbageCollector() = default;

void* jllvm::GarbageCollector::allocate(std::size_t size)
{
    bool attemptedGarbageCollected = false;
    do
    {
        if ((gcEveryAlloc && !attemptedGarbageCollected) || (m_bumpPtr - m_fromSpace) + size > m_heapSize)
        {
            if (!attemptedGarbageCollected)
            {
                attemptedGarbageCollected = true;
                garbageCollect();
                continue;
            }

            // TODO: throw out of memory exception
            llvm::report_fatal_error("Out of memory");
        }
        break;
    } while (true);

    void* result = m_bumpPtr;
    m_bumpPtr += size;
    m_bumpPtr += llvm::offsetToAlignedAddr(m_bumpPtr, llvm::Align(alignof(ObjectHeader)));
    return result;
}

void jllvm::GarbageCollector::addStackMapEntries(std::uintptr_t addr, llvm::ArrayRef<StackMapEntry> entries)
{
    if (entries.empty())
    {
        return;
    }
    LLVM_DEBUG({ llvm::dbgs() << "Added stackmap entries for PC " << (void*)addr << '\n'; });
    auto& vec = m_entries[addr];
    vec.insert(vec.end(), entries.begin(), entries.end());
}

void jllvm::GarbageCollector::RootProvider::addRootsForRelocation(RelocateObjectFn relocateObjectFn)
{
    addRootObjects(
        [=](ObjectInterface* object)
        {
            // Root objects are known to not be on the GC's heap but may contain references to GC objects.
            // Introspect the object to get its fields and consider them roots for relocation.
            introspectObject(object, [=](ObjectInterface** field) { relocateObjectFn(field); });
        });
}

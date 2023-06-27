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
class ObjectRepr
{
    llvm::PointerIntPair<jllvm::ClassObject*, 1, bool> m_classObject;

public:
    jllvm::ClassObject* getClass() const
    {
        return m_classObject.getPointer();
    }

    bool hasBeenSeen() const
    {
        return m_classObject.getInt();
    }

    void markSeen()
    {
        m_classObject.setInt(true);
    }

    void clearMark()
    {
        m_classObject.setInt(false);
    }

    std::size_t getSize() const;
};

static_assert(std::is_standard_layout_v<ObjectRepr>);

std::size_t ObjectRepr::getSize() const
{
    std::size_t instanceSize = getClass()->getInstanceSize();
    if (const jllvm::ClassObject* component = getClass()->getComponentType())
    {
        std::size_t length = reinterpret_cast<const jllvm::Array<>&>(*this).size();
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

bool shouldBeAddedToWorkList(ObjectRepr* repr, ObjectRepr* from, ObjectRepr* to)
{
    if (!(repr >= from && repr < to))
    {
        return false;
    }
    return !repr->hasBeenSeen();
}

void collectStackRoots(const llvm::DenseMap<std::uintptr_t, std::vector<jllvm::StackMapEntry>>& map,
                       std::vector<ObjectRepr*>& results, ObjectRepr* from, ObjectRepr* to)
{
    jllvm::unwindStack(
        [&](jllvm::UnwindFrame context)
        {
            for (const auto& iter : map.lookup(context.getProgramCounter()))
            {
                switch (iter.type)
                {
                    case jllvm::StackMapEntry::Register:
                    {
                        auto rp = context.getIntegerRegister(iter.registerNumber);
                        auto* object = reinterpret_cast<ObjectRepr*>(rp);
                        if (shouldBeAddedToWorkList(object, from, to))
                        {
                            results.push_back(object);
                            object->markSeen();
                        }
                        break;
                    }
                    case jllvm::StackMapEntry::Direct:
                    {
                        llvm_unreachable("We don't do stack allocations");
                    }
                    case jllvm::StackMapEntry::Indirect:
                    {
                        auto rp = context.getStackPointer();
                        auto** ptr = reinterpret_cast<ObjectRepr**>(rp + iter.offset);
                        for (std::size_t i = 0; i < iter.count; i++)
                        {
                            auto* object = ptr[i];
                            if (!shouldBeAddedToWorkList(object, from, to))
                            {
                                continue;
                            }
                            object->markSeen();
                            results.push_back(object);
                        }
                        break;
                    }
                }
            }
        });
}

void replaceStackRoots(const llvm::DenseMap<std::uintptr_t, std::vector<jllvm::StackMapEntry>>& map,
                       const llvm::DenseMap<ObjectRepr*, ObjectRepr*>& mapping)
{
    jllvm::unwindStack(
        [&](jllvm::UnwindFrame context)
        {
            for (const auto& iter : map.lookup(context.getProgramCounter()))
            {
                switch (iter.type)
                {
                    case jllvm::StackMapEntry::Register:
                    {
                        auto rp = context.getIntegerRegister(iter.registerNumber);
                        if (!rp)
                        {
                            break;
                        }
                        auto* object = reinterpret_cast<ObjectRepr*>(rp);
                        if (auto* replacement = mapping.lookup(object))
                        {
                            context.setIntegerRegister(iter.registerNumber,
                                                       reinterpret_cast<std::uintptr_t>(replacement));
                        }
                        break;
                    }
                    case jllvm::StackMapEntry::Direct:
                    {
                        llvm_unreachable("We don't do stack allocations");
                    }
                    case jllvm::StackMapEntry::Indirect:
                    {
                        auto rp = context.getStackPointer();
                        auto** ptr = reinterpret_cast<ObjectRepr**>(rp + iter.offset);
                        for (std::size_t i = 0; i < iter.count; i++)
                        {
                            auto* object = ptr[i];
                            if (!object)
                            {
                                continue;
                            }
                            if (ObjectRepr* replacement = mapping.lookup(object))
                            {
                                ptr[i] = replacement;
                            }
                        }
                        break;
                    }
                }
            }
        });
}

template <class F>
void introspectObject(ObjectRepr* object, F&& f)
{
    jllvm::ClassObject* classObject = object->getClass();
    if (const jllvm::ClassObject* componentType = classObject->getComponentType())
    {
        // Array of references.
        if (!componentType->isPrimitive())
        {
            auto* array = reinterpret_cast<jllvm::Array<ObjectRepr*>*>(object);
            for (ObjectRepr** iter = array->begin(); iter != array->end(); iter++)
            {
                f(iter);
            }
        }
        return;
    }

    for (std::uint32_t iter : classObject->getGCObjectMask())
    {
        f(reinterpret_cast<ObjectRepr**>(reinterpret_cast<char*>(object) + iter * sizeof(jllvm::Object*)));
    }
}

void mark(std::vector<ObjectRepr*>& workList, ObjectRepr* from, ObjectRepr* to)
{
    while (!workList.empty())
    {
        ObjectRepr* object = workList.back();
        workList.pop_back();

        introspectObject(object,
                         [&](ObjectRepr** field)
                         {
                             ObjectRepr* reached = *field;
                             if (shouldBeAddedToWorkList(reached, from, to))
                             {
                                 reached->markSeen();
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
    auto* from = reinterpret_cast<ObjectRepr*>(m_fromSpace);
    auto* to = reinterpret_cast<ObjectRepr*>(m_bumpPtr);

    std::vector<ObjectRepr*> roots;
    collectStackRoots(m_entries, roots, from, to);

    auto addToWorkListLambda = [&roots, from, to](void* ptr)
    {
        auto* object = reinterpret_cast<ObjectRepr*>(ptr);
        if (shouldBeAddedToWorkList(object, from, to))
        {
            object->markSeen();
            roots.push_back(object);
        }
    };
    llvm::for_each(llvm::make_pointee_range(m_staticRoots), addToWorkListLambda);
    for (RootFreeList& list : m_localRoots)
    {
        llvm::for_each(llvm::make_pointee_range(list), addToWorkListLambda);
    }

    mark(roots, from, to);

    auto nextObject = [](char* curr)
    {
        auto* object = reinterpret_cast<ObjectRepr*>(curr);
        curr += object->getSize();
        curr += llvm::offsetToAlignedAddr(curr, llvm::Align(alignof(ObjectHeader)));
        return curr;
    };

    [[maybe_unused]] std::size_t collectedObjects = 0;
    [[maybe_unused]] std::size_t relocatedObjects = 0;

    __asan_unpoison_memory_region(m_toSpace, m_heapSize);

    auto* oldBumpPtr = m_bumpPtr;
    m_bumpPtr = m_toSpace;
    std::memset(m_bumpPtr, 0, m_heapSize);
    llvm::DenseMap<ObjectRepr*, ObjectRepr*> mapping;
    for (char* iter = m_fromSpace; iter != oldBumpPtr; iter = nextObject(iter))
    {
        auto* object = reinterpret_cast<ObjectRepr*>(iter);
        auto objectSize = object->getSize();
        if (!object->hasBeenSeen())
        {
            collectedObjects++;
            continue;
        }

        relocatedObjects++;
        object->clearMark();
        auto* newStorage = m_bumpPtr;
        m_bumpPtr += objectSize;
        m_bumpPtr += llvm::offsetToAlignedAddr(m_bumpPtr, llvm::Align(alignof(ObjectHeader)));
        std::memcpy(newStorage, object, objectSize);
        mapping[object] = reinterpret_cast<ObjectRepr*>(newStorage);
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

    for (void** root : m_staticRoots)
    {
        if (ObjectRepr* replacement = mapping.lookup(reinterpret_cast<ObjectRepr*>(*root)))
        {
            *root = replacement;
        }
    }

    for (RootFreeList& list : m_localRoots)
    {
        for (void** root : list)
        {
            if (ObjectRepr* replacement = mapping.lookup(reinterpret_cast<ObjectRepr*>(*root)))
            {
                *root = replacement;
            }
        }
    }

    for (char* iter = m_fromSpace; iter != m_bumpPtr; iter = nextObject(iter))
    {
        auto* object = reinterpret_cast<ObjectRepr*>(iter);

        introspectObject(object,
                         [&](ObjectRepr** field)
                         {
                             if (ObjectRepr* replacement = mapping.lookup(*field))
                             {
                                 *field = replacement;
                             }
                         });
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

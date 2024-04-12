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

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>

#include <jllvm/object/ClassObject.hpp>
#include <jllvm/object/Object.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "RootFreeList.hpp"

namespace jllvm
{
struct StackMapEntry
{
    /// Base pointer which points directly at an object.
    WriteableFrameValue<ObjectInterface*> basePointer;
    /// Derived pointer which may be at an offset to the base pointer and therefore possibly point into the middle of
    /// the object. After relocation, it should have the same offset from the relocated base pointer as it did prior to
    /// relocation.
    WriteableFrameValue<std::byte*> derivedPointer;
};

class GarbageCollector;

/// Owning version of 'GCRootRef' used to own and automatically free GC roots created by the GCs 'root' method on
/// destruction.
/// These should be the primary mechanism used in C++ code to retain Java objects beyond garbage collections.
template <class T>
class GCUniqueRoot : public GCRootRef<T>
{
    using Base = GCRootRef<T>;

    friend class GarbageCollector;

    GarbageCollector* m_gc = nullptr;

    explicit GCUniqueRoot(GarbageCollector* gc, GCRootRef<T> object) : GCRootRef<T>(object), m_gc(gc) {}

public:
    ~GCUniqueRoot()
    {
        reset();
    }

    GCUniqueRoot(const GCUniqueRoot&) = delete;
    GCUniqueRoot& operator=(const GCUniqueRoot&) = delete;

    /// Transfers ownership of the root from 'rhs' to this newly constructed root.
    /// 'rhs' is an empty root afterwards.
    GCUniqueRoot(GCUniqueRoot&& rhs) noexcept : Base(rhs.release()), m_gc(rhs.m_gc) {}

    GCUniqueRoot& operator=(GCUniqueRoot&& rhs) noexcept
    {
        // Reuse move constructor for move assignment.
        this->~GCUniqueRoot();
        return *new (this) GCUniqueRoot(std::move(rhs));
    }


    /// Releases ownership of the root from this object, returning it as 'GCRootRef'.
    /// The object is contains an empty root afterwards.
    GCRootRef<T> release()
    {
        GCRootRef<T> copy = *this;
        static_cast<Base&>(*this) = nullptr;
        return copy;
    }

    /// Deletes the root managed by this 'GCUniqueRoot'. Any other 'GCRootRef's referring to the root are left dangling.
    /// The object is left in an invalid state as described in the move constructor description.
    void reset();
};

/// Garbage collector and heap implementation used to allocate and free ALL Java objects minus class objects.
/// This is a simple semi-space collector with a bump pointer to do new allocations. It consist of both a 'from' and
/// 'to' space, each equal to the heap size. A garbage collection simply consists of copying all objects that are still
/// alive from the 'from' to the 'to' space and then switching the 'from' and 'to' space designation.
///
/// Objects referred to on the stack by Java methods are generally automatically relocated by the garbage collector
/// and do not need to be handled specially.
///
/// When referring to Java objects from C++ special care has to be taken when interacting with the GC.
/// One may use one of 'T*', 'GCRootRef' or 'GCUniqueRoot' to refer to Java objects from C++ with following caveats:
///
/// 'T*' aka "Pointer to Java objects":
/// Pointer to Java objects in the C++ stack are: 1) NOT visible to the GC and 2) NOT relocated by the GC.
/// This has the consequence that an object still referred to by a C++ pointer possibly being freed anyways and
/// the pointer being left dangling after garbage collection due to the object having been relocated.
///
/// It is therefore only save to use 'T*' when not doing any operation that may possibly trigger garbage collection.
/// Using 'T*' has the advantage of having zero runtime overhead and is also required when calling C++ code from
/// LLVM IR (not JNI).
///
/// 'GCUniqueRoot':
/// Roots are objects handed out by the GC with which it is possible from C++ code to refer to Java objects without
/// any of the disadvantages listed in the 'T*' sections. It is handled specially by the GC and used to determine
/// whether a Java object is still reachable, and gets updated by the GC when the object is relocated.
///
/// Roots have a lifetime and slight overhead on allocation and deallocation compared to 'T*'. Access is generally
/// optimized well by the compiler if it knows that no garbage collection may occur inbetween accesses.
///
/// 'GCUniqueRoot' is an owning version of a root with unique ownership. It can therefore not be copied and should
/// generally be passed to methods as 'GCUniqueRoot&&' if transferring ownership.
///
/// The non owning version of 'GCUniqueRoot' is 'GCRootRef'. It has the exact same advantages as 'GCUniqueRoot' and is
/// a more light weight object that can be copied around and should be passed by value. It does not ensure the lifetime
/// of its root, requiring it to be managed externally instead.
/// It should generally be preferred over 'GCUniqueRoot' when passing around as arguments and not transferring
/// ownership.
/// A more useful analogy: 'GCRootRef' is to 'GCUniqueRoot' what 'std::string_view/llvm::StringRef' is to 'std::string'.
///
/// Local root frames:
/// Due to how different access patterns in typical programs are, two different root lists exist which manage the
/// lifetime of their roots differently:
/// * Global roots returned by 'allocateStatic'
/// * Local roots returned by 'root'.
///
/// The former has global lifetime while the latter has the concept of 'frames'.
///
/// A new local root frame is created by using 'pushLocalFrame'. All subsequent 'root' operations are then allocated
/// within this frame. Once 'popLocalFrame' is called, all roots that had been allocated in that frame with 'root' are
/// freed. It is also possible to delete a root early using `deleteRoot' which is used by 'GCUniqueRoot'.
///
/// There is always at least one local frame available, making 'root' always safe to use.
/// A new local frame is pushed for every Java to Native transition by the JNI and popped again when returning to the
/// Java function.
class GarbageCollector
{
    std::size_t m_heapSize;
    std::unique_ptr<char[]> m_spaceOne;
    std::unique_ptr<char[]> m_spaceTwo;

    char* m_fromSpace;
    char* m_toSpace;

    char* m_bumpPtr;

    llvm::DenseMap<std::uintptr_t, std::vector<StackMapEntry>> m_entries;

    static constexpr auto LOCAL_SLAB_SIZE = 64;

    // Roots for static fields of classes.
    RootFreeList m_staticRoots;
    // Local roots for other C++ code. Generally has a very different allocation pattern than static fields, hence kept
    // separate.
    std::vector<RootFreeList> m_localRoots;

public:
    /// Interface called by the GC allowing adding roots and objects allocated in heaps outside of the GC's heap to the
    /// marking and relocation process. This is required for correctness as the GC needs to be aware about the
    /// reachability of all objects. If an object 'O' is not part of the GC's heap, but refers to objects on the GC's
    /// heap it would not be counted as reachable unless 'O' is added to the mark phase through a 'RootProvider'.
    class RootProvider
    {
    public:
        RootProvider() = default;

        virtual ~RootProvider() = default;
        RootProvider(const RootProvider&) = delete;
        RootProvider& operator=(const RootProvider&) = delete;
        RootProvider(RootProvider&&) = delete;
        RootProvider& operator=(RootProvider&&) = delete;

        using AddRootObjectFn = llvm::function_ref<void(ObjectInterface*)>;
        using RelocateObjectFn = llvm::function_ref<void(ObjectInterface*&)>;

        /// Called to add additional roots to the mark phase by calling 'relocateObjectFn'.
        /// Objects pointed to by roots are marked as reachable by the GC and are updated by the GC as it relocates
        /// objects. Note that the method may be called multiple times during one garbage collection and must provide
        /// the same set of roots each time.
        ///
        /// Default implementation calls 'addRootObjects' and should be called explicitly in any subclasses if
        /// 'addRootObjects' is overwritten as well.
        virtual void addRootsForRelocation(RelocateObjectFn relocateObjectFn);

        /// Called to add an external object from outside the GC's heap to the marking phase. This is required if such
        /// an object may point to objects on the GC's heap. Failing to do so will lead to GC heap objects being deleted
        /// despite still in use or relocated without updating any references in the external object.
        virtual void addRootObjects(AddRootObjectFn /*addRootObjectFn*/)
        {
            llvm_unreachable("expected either 'addRootsForRelocation' or 'addRootObjects' to be overwritten");
        }
    };

private:
    std::vector<std::unique_ptr<RootProvider>> m_rootProviders;

    template <class T>
    struct IsArray : std::false_type
    {
    };

    template <class T>
    struct IsArray<Array<T>> : std::true_type
    {
    };

public:
    /// Creates the garbage collector with the given heap size. The GC does garbage collection once the heap is too
    /// large to support another allocation.
    GarbageCollector(std::size_t heapSize);

    ~GarbageCollector();

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;
    GarbageCollector(GarbageCollector&&) = delete;
    GarbageCollector& operator=(GarbageCollector&&) = delete;

    /// Allocates a new object with 'size' size. The returned object is always pointer aligned.
    void* allocate(std::size_t size);

    /// Allocates a new instance of 'classObject', constructing it with 'classObject' followed by 'args'.
    template <class T = Object, class... Args>
    T* allocate(const ClassObject* classObject, Args&&... args)
        requires(std::is_base_of_v<ObjectInterface, T> && !IsArray<T>{} && !std::same_as<T, AbstractArray>)
    {
        assert(classObject->isClass());
        return new (allocate(classObject->getInstanceSize())) T(classObject, std::forward<Args>(args)...);
    }

    /// Allocates a new array of type 'classObject' containing 'length' amount of elements.
    template <class T>
    T* allocate(const ClassObject* classObject, std::uint32_t length) requires(IsArray<T>::value)
    {
        assert(classObject->isArray());
        return new (allocate(T::arrayElementsOffset() + sizeof(typename T::value_type) * length))
            T(classObject, length);
    }

    /// Allocates a new array of type 'classObject' containing 'length' amount of elements.
    /// This differs from other overloads by not requiring specifying the element type of the array but reading it from
    /// the class object instead.
    template <std::same_as<AbstractArray> T>
    AbstractArray* allocate(const ClassObject* classObject, std::uint32_t length)
    {
        assert(classObject->isArray());
        return selectForJVMType(classObject->getComponentType()->getDescriptor(),
                                [&]<class C>(C) -> AbstractArray* { return allocate<Array<C>>(classObject, length); });
    }

    /// Pushes a new local frame onto the internal stack, making it the currently active frame.
    /// All subsequent 'root' operations allocate within this frame.
    void pushLocalFrame()
    {
        m_localRoots.emplace_back(LOCAL_SLAB_SIZE);
    }

    /// Allocates a new local root in the currently active local frame with which references to Java objects can be
    /// persisted.
    /// Initializes the root to refer to 'object'.
    template <std::derived_from<ObjectInterface> T>
    GCUniqueRoot<T> root(T* object = nullptr)
    {
        GCUniqueRoot<T> uniqueRoot(this, static_cast<GCRootRef<T>>(m_localRoots.back().allocate()));
        uniqueRoot.assign(object);
        return uniqueRoot;
    }

    /// Manual deletion method for roots returned by 'root'.
    /// This method currently requires that the local frame in which the root was created is currently active.
    /// Calling this method on a root not created by 'root', calling it on a root twice or calling it on a root
    /// whose frame has already been deleted is undefined behaviour.
    ///
    /// NOTE: This should generally not be called by the user, as the 'GCUniqueRoot' returned by 'root' does so
    /// automatically.
    void deleteRoot(GCRootRef<ObjectInterface> root)
    {
        m_localRoots.back().free(root);
    }

    /// Pops the currently active local frame from the internal stack, making the previous frame active again.
    /// Calling this method without a unique corresponding 'pushLocalFrame' operation is undefined behaviour.
    void popLocalFrame()
    {
        assert(m_localRoots.size() > 1 && "Can't pop frame not explicitly pushed");
        m_localRoots.pop_back();
    }

    /// Adds a new 'RootProvider' to the GC.
    void addRootProvider(std::unique_ptr<RootProvider>&& rootProvider)
    {
        m_rootProviders.push_back(std::move(rootProvider));
    }

    /// Adds a new 'RootProvider' implementing 'addRootObjects' with the given callable.
    template <std::invocable<RootProvider::AddRootObjectFn> F>
    void addRootObjectsProvider(F&& f)
    {
        struct Provider final : std::decay_t<F>, RootProvider
        {
            using Base = std::decay_t<F>;

            Provider(F&& f) : Base(std::forward<F>(f)) {}

            void addRootObjects(RootProvider::AddRootObjectFn addRootObjectFn) override
            {
                (*this)(addRootObjectFn);
            }
        };
        addRootProvider(std::make_unique<Provider>(std::forward<F>(f)));
    }

    /// Adds a new 'RootProvider' implementing 'addRootsForRelocation' with the given callable.
    template <std::invocable<RootProvider::RelocateObjectFn> F>
    void addRootsForRelocationProvider(F&& f)
    {
        struct Provider final : std::decay_t<F>, RootProvider
        {
            using Base = std::decay_t<F>;

            Provider(F&& f) : Base(std::forward<F>(f)) {}

            void addRootsForRelocation(RootProvider::RelocateObjectFn relocateObjectFn) override
            {
                (*this)(relocateObjectFn);
            }
        };
        addRootProvider(std::make_unique<Provider>(std::forward<F>(f)));
    }

    /// Adds new stack map entries to the garbage collector, allowing the garbage collector to read out any alive
    /// stack variable references at the given instruction pointer address. Called by the JIT.
    void addStackMapEntries(std::uintptr_t addr, llvm::ArrayRef<StackMapEntry> entries);

    /// Allocates a new static field of reference type within the GC. The GC additionally manages this heap to be able
    /// to both use it as root objects during marking and to properly replace references to relocated objects during
    /// sweeping.
    GCRootRef<Object> allocateStatic();

    /// Returns the size of the object heap in bytes.
    std::size_t getHeapSize() const
    {
        return m_heapSize;
    }

    void garbageCollect();
};

template <class T>
void GCUniqueRoot<T>::reset()
{
    if (!this->hasRoot())
    {
        return;
    }

    m_gc->deleteRoot(release());
}

} // namespace jllvm

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>

#include <jllvm/object/ClassObject.hpp>
#include <jllvm/object/Object.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "RootFreeList.hpp"

namespace jllvm
{
struct StackMapEntry
{
    enum Type : std::uint8_t
    {
        Register = 1,
        Direct = 2,
        Indirect = 3,
    } type;
    std::uint8_t count;
    int registerNumber;
    std::uint32_t offset;

    bool operator==(const StackMapEntry& rhs) const
    {
        return type == rhs.type && count == rhs.count && registerNumber == rhs.registerNumber && offset == rhs.offset;
    }

    bool operator<(const StackMapEntry& rhs) const
    {
        return std::tie(type, count, registerNumber, offset)
               < std::tie(rhs.type, rhs.count, rhs.registerNumber, rhs.offset);
    }
};

class GarbageCollector;

/// Owning version of 'GCRootRef' used to own and automatically free GC roots created by the GCs 'root' method on
/// destruction.
/// These should be the primary mechanism used in C++ code to retain Java objects beyond garbage collections.
template <class T>
class GCUniqueRoot : public GCRootRef<T>
{
    friend class GarbageCollector;

    GarbageCollector* m_gc = nullptr;

    explicit GCUniqueRoot(GarbageCollector* gc, GCRootRef<T> object) : GCRootRef<T>(object), m_gc(gc) {}

public:
    /// Allows assignment from a valid pointer to an object.
    GCUniqueRoot& operator=(T* object)
    {
        *this->m_object = object;
        return *this;
    }

    ~GCUniqueRoot()
    {
        reset();
    }

    GCUniqueRoot(const GCUniqueRoot&) = delete;
    GCUniqueRoot& operator=(const GCUniqueRoot&) = delete;

    /// Transfers ownership of the root from 'rhs' to this newly constructed root.
    /// 'rhs' is left in an invalid state on which no methods but the destructor, 'data' and the move assignment
    /// operator are valid.
    GCUniqueRoot(GCUniqueRoot&& rhs) noexcept
        : GCRootRef<T>(std::exchange(rhs.m_object, nullptr)), m_gc(std::exchange(rhs.m_gc, nullptr))
    {
    }

    /// Assignment version of the move constructor. See the move constructor.
    GCUniqueRoot& operator=(GCUniqueRoot&& rhs) noexcept
    {
        reset();

        this->m_object = std::exchange(rhs.m_object, nullptr);
        m_gc = std::exchange(rhs.m_gc, nullptr);
        return *this;
    }

    /// Releases ownership of the root from this object, returning it as 'GCRootRef'.
    /// The object is left in an invalid state as described in the move constructor description.
    GCRootRef<T> release()
    {
        GCRootRef<T> copy = *this;
        m_gc = nullptr;
        this->m_object = nullptr;
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
class GarbageCollector
{
    std::size_t m_heapSize;
    std::unique_ptr<char[]> m_spaceOne;
    std::unique_ptr<char[]> m_spaceTwo;

    char* m_fromSpace;
    char* m_toSpace;

    char* m_bumpPtr;

    llvm::DenseMap<std::uintptr_t, std::vector<StackMapEntry>> m_entries;

    // Roots for static fields of classes.
    RootFreeList m_staticRoots;
    // Local roots for other C++ code. Generally has a very different allocation pattern than static fields, hence kept
    // separate.
    RootFreeList m_localRoots;

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
        requires(std::is_base_of_v<ObjectInterface, T> && !IsArray<T>{})
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

    /// Allocates a new root with which references to Java objects can be persisted.
    /// Initializes the root to refer to 'object'.
    template <std::derived_from<ObjectInterface> T>
    GCUniqueRoot<T> root(T* object = nullptr)
    {
        GCUniqueRoot<T> uniqueRoot(this, static_cast<GCRootRef<T>>(m_localRoots.allocate()));
        uniqueRoot = object;
        return uniqueRoot;
    }

    /// Adds new stack map entries to the garbage collector, allowing the garbage collector to read out any alive
    /// stack variable references at the given instruction pointer address. Called by the JIT.
    void addStackMapEntries(std::uintptr_t addr, llvm::ArrayRef<StackMapEntry> entries);

    /// Allocates a new static field of reference type within the GC. The GC additionally manages this heap to be able
    /// to both use it as root objects during marking and to properly replace references to relocated objects during
    /// sweeping.
    GCRootRef<Object> allocateStatic();

    void garbageCollect();

    /// Manual deletion method for roots returned by 'root'.
    /// NOTE: This should generally not be called by the user, as the 'GCUniqueRoot' returned by 'root' does so
    /// automatically.
    /// Calling this method on a root not created by 'root' or calling it on a root twice is undefined behaviour.
    void deleteRoot(GCRootRef<ObjectInterface> root)
    {
        m_localRoots.free(root);
    }
};

template <class T>
void GCUniqueRoot<T>::reset()
{
    if (!m_gc)
    {
        return;
    }
    m_gc->deleteRoot(*this);
    m_gc = nullptr;
    this->m_object = nullptr;
}

} // namespace jllvm

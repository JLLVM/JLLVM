#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>

#include <jllvm/object/Object.hpp>

#include <cstdint>
#include <memory>
#include <vector>

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

template <class T>
class GCRef;

/// Garbage collector and heap implementation used to allocate and free ALL Java objects minus class objects.
/// This is a simple semi-space collector with a bump pointer to do new allocations. It consist of both a 'from' and
/// 'to' space, each equal to the heap size. A garbage collection simply consists of copying all objects that are still
/// alive from the 'from' to the 'to' space and then switching the 'from' and 'to' space designation.
class GarbageCollector
{
    std::size_t m_heapSize;
    std::unique_ptr<char[]> m_spaceOne;
    std::unique_ptr<char[]> m_spaceTwo;

    char* m_fromSpace;
    char* m_toSpace;

    char* m_bumpPtr;

    llvm::DenseMap<std::uintptr_t, std::vector<StackMapEntry>> m_entries;

    std::vector<std::unique_ptr<void*[]>> m_staticRefsSlabs;
    void** m_staticRefsBumpPtr = nullptr;

    void garbageCollect();

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

    /// Adds new stack map entries to the garbage collector, allowing the garbage collector to read out any alive
    /// stack variable references at the given instruction pointer address. Called by the JIT.
    void addStackMapEntries(std::uintptr_t addr, llvm::ArrayRef<StackMapEntry> entries);

    /// Allocates a new static field of reference type within the GC. The GC additionally manages this heap to be able
    /// to both use it as root objects during marking and to properly replace references to relocated objects during
    /// sweeping.
    GCRef<Object> allocateStatic();
};

template <class T>
class GCRef
{
    T** m_object;

public:
    explicit GCRef(void** object) : m_object(reinterpret_cast<T**>(object)) {}

    template <class U>
    explicit operator GCRef<U>() const
    {
        return GCRef<U>(reinterpret_cast<void**>(m_object));
    }

    explicit operator bool() const
    {
        return get();
    }

    bool operator!() const
    {
        return !get();
    }

    T* get() const
    {
        return *m_object;
    }

    T* operator->() const
    {
        return get();
    }

    T& operator*() const
    {
        return *get();
    }

    T** ref() const
    {
        return m_object;
    }
};
} // namespace jllvm

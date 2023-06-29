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

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/iterator.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace jllvm
{

class ObjectInterface;

/// Class used to refer to a so called "root" allocated by a 'RootFreeList', usually by the garbage collectors.
/// This class can be seen as a GC-Safe equivalent to a pointer for C++ code, allowing one to refer to an object
/// and continue referring to the object even after the object has been relocated by the garbage collector.
/// It also has the important role of keeping the object it refers to alive, preventing it from being deleted by the
/// garbage collector.
///
/// Most importantly, this class does not manage the lifetime of the root but relies on an outside mechanism to keep
/// the underlying root alive.
/// Rather its a lightweight reference type that should be passed by value, analogous to what 'std::string_view'
/// is to 'std::string' (the equivalent of 'std::string' being 'GCUniqueRoot' in this analogy).
/// The primary mechanism to create roots are by using the 'root' method of the 'GarbageCollector'.
///
/// See its documentation as to when to use 'GCRootRef', 'GCUniqueRoot' or a plain 'T*'.
///
/// 'T' is the memory layout of the Java object that should be used when retrieving it.
template <class T = ObjectInterface>
class GCRootRef
{
protected:
    void** m_object = nullptr;

    friend class RootFreeList;

    template <class U>
    friend class GCRootRef;

    explicit GCRootRef(void** object) : m_object(object) {}

    T* get() const
    {
        return reinterpret_cast<T*>(*m_object);
    }

public:
    /// Creates a new 'GCRootRef' from a derived 'GCRootRef', referring to the same root.
    template <std::derived_from<T> U>
    GCRootRef(GCRootRef<U> rhs) requires(!std::is_same_v<T, U>) : m_object(rhs.m_object)
    {
    }

    /// Explicit cast to a 'GCRootRef' of another type. This allows both up and down casting and does not perform any
    /// validity checks. It is therefore up to the user to make sure the cast is valid.
    template <class U>
    explicit operator GCRootRef<U>() const
    {
        return GCRootRef<U>(m_object);
    }

    /// Explicit cast to 'T*'. This operation should generally be avoided in favour of just using the 'GCRootRef' as
    /// intended.
    explicit operator T*() const
    {
        return get();
    }

    /// Allows assignment from a valid pointer to an object.
    GCRootRef& operator=(T* object)
    {
        *m_object = const_cast<std::remove_const_t<T>*>(object);
        return *this;
    }

    /// Returns true if 'lhs' and 'rhs' refer to the same object.
    template <class U>
    friend bool operator==(GCRootRef<T> lhs, GCRootRef<U> rhs)
    {
        return lhs.get() == static_cast<U*>(rhs);
    }

    /// Returns true if 'lhs' and 'rhs' refer to the same object.
    friend bool operator==(GCRootRef<T> lhs, const T* object)
    {
        return lhs.get() == object;
    }

    /// Returns true if this root contains a reference to an object instead of null.
    explicit operator bool() const
    {
        return get();
    }

    /// Returns true if this root contains null instead of a reference to an object.
    bool operator!() const
    {
        return !get();
    }

    /// Allows using a 'GCRootRef<T>' with the same syntax as you would a 'T*'
    T* operator->() const
    {
        return get();
    }

    T& operator*() const
    {
        return *get();
    }

    /// Returns the address of the Java object.
    /// The address is only valid until the next garbage collection.
    T* address() const
    {
        return get();
    }

    /// Returns the underlying root referred to by this 'GCRootRef'.
    void** data() const
    {
        return m_object;
    }
};

/// Special allocator used to allocate new root objects.
/// It is optimized for LIFO order of allocation and de-allocation patterns.
/// Internally, it uses slabs of memory with an inplace singly linked list keeping track of freed slots available for
/// allocation.
/// Additionally, it allows iterating over all currently still alive roots.
class RootFreeList
{
    std::size_t m_slabSize;
    std::vector<std::unique_ptr<void*[]>> m_slabs;
    std::size_t m_currentSlab = 0;
    void** m_freeListNext = nullptr;
    void** m_freeListEnd = nullptr;

    class SlotsIterator : public llvm::iterator_facade_base<SlotsIterator, std::forward_iterator_tag, void**,
                                                            std::ptrdiff_t, void***, void**>
    {
        std::size_t m_slabSize = 0;
        const std::unique_ptr<void*[]>* m_currentSlab = nullptr;
        std::size_t m_current = 0;

    public:
        SlotsIterator() = default;

        SlotsIterator(std::size_t slabSize, const std::unique_ptr<void*[]>* currentSlab, void** current)
            : m_slabSize(slabSize), m_currentSlab(currentSlab), m_current(current - currentSlab->get())
        {
            if (m_current == m_slabSize)
            {
                m_currentSlab++;
                m_current = 0;
            }
        }

        bool operator==(const SlotsIterator& rhs) const
        {
            return m_current == rhs.m_current && m_currentSlab == rhs.m_currentSlab;
        }

        reference operator*() const
        {
            return (*m_currentSlab).get() + m_current;
        }

        SlotsIterator& operator++()
        {
            m_current++;
            if (m_current == m_slabSize)
            {
                m_current = 0;
                m_currentSlab++;
            }
            return *this;
        }
    };

    struct FilterPredicate
    {
        bool operator()(void** pointer) const noexcept
        {
            // Check whether the root is an alive root or a free slot.
            // In the latter case it is marked with a set LSB which is never the case for used roots since Object's are
            // pointer aligned.
            return !(reinterpret_cast<std::uintptr_t>(*pointer) & 1);
        }
    };

public:
    /// Creates a new root free list with the given amount of roots per slab.
    explicit RootFreeList(std::size_t slabSize) : m_slabSize(slabSize)
    {
        m_slabs.push_back(std::make_unique<void*[]>(m_slabSize));
        m_freeListNext = m_freeListEnd = m_slabs[m_currentSlab].get();
    }

    /// Allocates a new root and returns it as 'GCRootRef'.
    /// The lifetime of the root remains valid until 'free' is called on the 'GCRootRef' of this method.
    GCRootRef<ObjectInterface> allocate();

    /// Frees a root created by this 'RootFreeList's 'allocate' method. This allows the root to be reused by any
    /// subsequent 'allocate' calls.
    /// If 'root' was not allocated by this 'RootFreeList's 'allocate' method or already freed the behaviour is
    /// undefined.
    void free(GCRootRef<ObjectInterface> root);

    /// Begin iterator over all alive roots.
    auto begin() const
    {
        auto begin = SlotsIterator(m_slabSize, m_slabs.data(), m_slabs.front().get());
        auto end = SlotsIterator(m_slabSize, m_slabs.data() + m_currentSlab, m_freeListEnd);
        return llvm::filter_iterator<SlotsIterator, FilterPredicate>(begin, end, {});
    }

    /// End iterator over all alive roots.
    auto end() const
    {
        auto end = SlotsIterator(m_slabSize, m_slabs.data() + m_currentSlab, m_freeListEnd);
        return llvm::filter_iterator<SlotsIterator, FilterPredicate>(end, end, {});
    }
};
} // namespace jllvm

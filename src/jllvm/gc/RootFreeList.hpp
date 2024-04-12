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
    ObjectInterface** m_root = nullptr;

    template <class U>
    friend class GCRootRef;

    T* get() const
    {
        if (!hasRoot())
        {
            return nullptr;
        }
        return static_cast<T*>(*m_root);
    }

public:
    /// Creates a new 'GCRootRef' from a root. The lifetime of the root must be valid throughout the use of the
    /// 'GCRootRef'.
    explicit GCRootRef(ObjectInterface** root) : m_root(root) {}

    /// Creates a 'GCRootRef' with no root. 'GCRootRef' without roots are for all intents and purposes equal to
    /// 'GCRootRef's that refer to a null reference with the exception of NOT being able to assign an object to it.
    /// Prefer assignment from another 'GCRootRef' instead.
    GCRootRef() = default;

    /// Allows implicit construction of a 'GCRootRef' without a root from a nullptr.
    GCRootRef(std::nullptr_t) : GCRootRef() {}

    /// Creates a new 'GCRootRef' from a derived 'GCRootRef', referring to the same root.
    template <std::derived_from<T> U>
    GCRootRef(GCRootRef<U> rhs) requires(!std::is_same_v<T, U>) : m_root(rhs.m_root)
    {
    }

    /// Assignment operator from a derived 'GCRootRef'.
    template <std::derived_from<T> U>
    GCRootRef& operator=(GCRootRef<U> rhs) requires(!std::is_same_v<T, U>)
    {
        *this = static_cast<GCRootRef<T>>(rhs);
        return *this;
    }

    /// Explicit cast to a 'GCRootRef' of another type. This allows both up and down casting and does not perform any
    /// validity checks. It is therefore up to the user to make sure the cast is valid.
    template <class U>
    explicit operator GCRootRef<U>() const
    {
        return GCRootRef<U>(m_root);
    }

    /// Explicit cast to 'T*'. This operation should generally be avoided in favour of just using the 'GCRootRef' as
    /// intended.
    /*implicit*/ operator T*() const
    {
        return get();
    }

    /// Returns true if this 'GCRootRef' has a root.
    bool hasRoot() const
    {
        return m_root;
    }

    /// Assign an object to the root of this 'GCRootRef'. This is only a valid operation if the 'GCRootRef' is not
    /// empty i.e. refers to a root.
    void assign(T* object)
    {
        assert(hasRoot() && "GCRootRef must have a root");
        *m_root = const_cast<std::remove_const_t<T>*>(object);
    }

    /// Returns true if 'lhs' and 'rhs' refer to the same object.
    template <class U>
    friend bool operator==(GCRootRef<T> lhs, GCRootRef<U> rhs)
    {
        return lhs.get() == static_cast<U*>(rhs);
    }

    /// Returns true if this root contains a reference to an object instead of null.
    friend bool operator==(GCRootRef<T> lhs, std::nullptr_t)
    {
        return !static_cast<bool>(lhs);
    }

    /// Returns true if this root contains a reference to an object instead of null.
    explicit operator bool() const
    {
        return get();
    }

    /// Returns true if this root does not contain a reference to an object.
    bool operator!() const
    {
        return !static_cast<bool>(*this);
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
    ObjectInterface** data() const
    {
        return m_root;
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
    std::vector<std::unique_ptr<ObjectInterface*[]>> m_slabs;
    std::size_t m_currentSlab = 0;
    ObjectInterface** m_freeListNext = nullptr;
    ObjectInterface** m_freeListEnd = nullptr;

    class SlotsIterator : public llvm::iterator_facade_base<SlotsIterator, std::forward_iterator_tag, ObjectInterface*>
    {
        std::size_t m_slabSize = 0;
        const std::unique_ptr<ObjectInterface*[]>* m_currentSlab = nullptr;
        std::size_t m_current = 0;

    public:
        SlotsIterator() = default;

        SlotsIterator(std::size_t slabSize, const std::unique_ptr<ObjectInterface*[]>* currentSlab,
                      ObjectInterface** current)
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
            return *(m_currentSlab->get() + m_current);
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
        bool operator()(ObjectInterface* pointer) const noexcept
        {
            // Check whether the root is an alive root or a free slot.
            // In the latter case it is marked with a set LSB which is never the case for used roots since Object's are
            // pointer aligned.
            return !(reinterpret_cast<std::uintptr_t>(pointer) & 0b1);
        }
    };

public:
    /// Creates a new root free list with the given amount of roots per slab.
    explicit RootFreeList(std::size_t slabSize) : m_slabSize(slabSize)
    {
        m_slabs.push_back(std::make_unique<ObjectInterface*[]>(m_slabSize));
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

// GCRootRefs convert to their contained object.
template <class T>
T* javaConvertedType(GCRootRef<T>);
} // namespace jllvm

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

#include <jllvm/object/GCRootRef.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace jllvm
{

class ObjectInterface;

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
} // namespace jllvm

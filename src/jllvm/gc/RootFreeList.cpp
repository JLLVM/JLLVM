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

#include "RootFreeList.hpp"

#include <cstring>

jllvm::GCRootRef<jllvm::ObjectInterface> jllvm::RootFreeList::allocate()
{
    // Optimized pattern for LIFO.
    if (m_freeListNext == m_freeListEnd)
    {
        // Check whether we need a new slab of memory.
        if (m_freeListNext == m_slabs[m_currentSlab].get() + m_slabSize)
        {
            if (++m_currentSlab == m_slabs.size())
            {
                m_slabs.push_back(std::make_unique<void*[]>(m_slabSize));
            }
            m_freeListEnd = m_freeListNext = m_slabs[m_currentSlab].get();
        }

        void** result = m_freeListNext;
        m_freeListNext = ++m_freeListEnd;

        *result = nullptr;
        return GCRootRef<ObjectInterface>(result);
    }

    // Remove head form singly linked list.
    void** result = m_freeListNext;
    auto next = reinterpret_cast<std::uintptr_t>(*m_freeListNext);
    // Remove free slot marker in LSB bit.
    m_freeListNext = reinterpret_cast<void**>(next & ~static_cast<std::uintptr_t>(1));

    *result = nullptr;
    return GCRootRef<ObjectInterface>(result);
}

void jllvm::RootFreeList::free(GCRootRef<ObjectInterface> root)
{
    void** raw = root.data();
    // LIFO optimized case.
    if (m_freeListNext == m_freeListEnd && raw + 1 == m_freeListNext)
    {
        m_freeListNext = --m_freeListEnd;
        if (m_currentSlab > 0 && m_freeListNext == m_slabs[m_currentSlab].get())
        {
            // Jump back to previous slab to allow further freeing its roots.
            m_freeListNext = m_freeListEnd = m_slabs[--m_currentSlab].get() + m_slabSize;
        }
        return;
    }

    *raw = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(m_freeListNext) | 1);
    m_freeListNext = raw;
}

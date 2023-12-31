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
#include <llvm/Support/Endian.h>

#include <concepts>

namespace jllvm
{

/// Special version of 'llvm::ArrayRef' operating on raw bytes and interpreting each value as a big-endian encoding of
/// 'T'.
template <std::integral T>
class BigEndianArrayRef
{
    const char* m_data;
    std::size_t m_size;

    class ConstProxy
    {
    protected:
        const char* m_data;

    public:
        explicit ConstProxy(const char* data) : m_data(data) {}

        operator T() const
        {
            return llvm::support::endian::read<T, llvm::support::big, /*alignment=*/1>(m_data);
        }
    };

public:
    BigEndianArrayRef(const char* data, size_t size) : m_data(data), m_size(size) {}

    /// Returns the index-th value in the array.
    ConstProxy operator[](std::size_t index) const
    {
        assert(index < size());
        return *std::next(begin(), index);
    }

    class iterator : public llvm::iterator_facade_base<iterator, std::random_access_iterator_tag, T, std::ptrdiff_t,
                                                       ConstProxy, ConstProxy>
    {
        const char* m_data{};

    public:
        iterator() = default;

        explicit iterator(const char* data) : m_data(data) {}

        bool operator==(iterator rhs) const
        {
            return m_data == rhs.m_data;
        }

        bool operator<(iterator rhs) const
        {
            return m_data < rhs.m_data;
        }

        ConstProxy operator*() const
        {
            return ConstProxy(m_data);
        }

        std::ptrdiff_t operator-(iterator rhs) const
        {
            return (m_data - rhs.m_data) / sizeof(T);
        }

        iterator& operator+=(std::ptrdiff_t rhs)
        {
            m_data += rhs * sizeof(T);
            return *this;
        }

        iterator& operator-=(std::ptrdiff_t rhs)
        {
            m_data -= rhs * sizeof(T);
            return *this;
        }
    };

    /// Returns the number of elements in the array.
    std::size_t size() const
    {
        return m_size;
    }

    auto begin() const
    {
        return iterator(m_data);
    }

    auto end() const
    {
        return iterator(m_data + m_size * sizeof(T));
    }
};

} // namespace jllvm

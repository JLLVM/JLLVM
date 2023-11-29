
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
#include <llvm/Support/MathExtras.h>

#include <limits>

namespace jllvm
{

/// Read-only view of a buffer of 'IntegerType', interpreting it as a bitset of a given size.
template <class IntegerType = std::uint64_t>
requires std::is_unsigned_v<IntegerType> class BitArrayRef
{
protected:
    const IntegerType* m_bits;
    std::size_t m_size;

    constexpr static std::size_t numBits = std::numeric_limits<IntegerType>::digits;

public:

    /// Create a new 'BitArrayRef' from a given buffer with 'size' many bits.
    /// 'bits' must be an array of at least 'ceil(size / num_bits(IntegerType))'.
    BitArrayRef(const IntegerType* bits, std::size_t size) : m_bits(bits), m_size(size) {}

    using value_type = IntegerType;

    class iterator : public llvm::indexed_accessor_iterator<iterator, const IntegerType*, bool>
    {
        using Base = llvm::indexed_accessor_iterator<iterator, const IntegerType*, bool>;

    public:
        iterator(const IntegerType* base, ptrdiff_t index) : Base(base, index) {}

        bool operator*() const
        {
            return this->getBase()[this->getIndex() / numBits] & (1 << (this->getIndex() % numBits));
        }
    };

    /// Returns the begin iterator over all bits.
    iterator begin() const
    {
        return iterator(m_bits, 0);
    }

    /// Returns the end iterator over all bits.
    iterator end() const
    {
        return iterator(m_bits, m_size);
    }

    /// Returns the number of bits of this 'BitArrayRef'.
    std::size_t size() const
    {
        return m_size;
    }

    /// Returns the value of the 'index'th bit.
    bool operator[](std::size_t index) const
    {
        assert(index < m_size);
        return *std::next(begin(), index);
    }
};

/// Mutable view of a buffer of 'IntegerType', interpreting it as a bitset of a given size.
template <std::integral IntegerType = std::uint64_t>
class MutableBitArrayRef : public BitArrayRef<IntegerType>
{
    using Base = BitArrayRef<IntegerType>;

    class Proxy
    {
        IntegerType* m_bits;
        std::size_t m_index;

    public:
        Proxy(IntegerType* bits, size_t index) : m_bits(bits), m_index(index) {}

        operator bool() const
        {
            return m_bits[m_index / Base::numBits] & (1 << (m_index % Base::numBits));
        }

        const Proxy& operator=(bool value) const
        {
            if (value)
            {
                m_bits[m_index / Base::numBits] |= 1 << m_index % Base::numBits;
            }
            else
            {
                m_bits[m_index / Base::numBits] &= ~(1 << m_index % Base::numBits);
            }
            return *this;
        }
    };

public:
    MutableBitArrayRef(IntegerType* bits, std::size_t size) : Base(bits, size) {}

    class iterator : public llvm::indexed_accessor_iterator<iterator, IntegerType*, Proxy>
    {
        using Base = llvm::indexed_accessor_iterator<iterator, IntegerType*, Proxy>;

    public:
        iterator(IntegerType* base, ptrdiff_t index) : Base(base, index) {}

        Proxy operator*() const
        {
            return Proxy(this->getBase(), this->getIndex());
        }
    };

    /// Returns the begin iterator over all bits. The iterator returns a proxy object which implicitly converts to a
    /// 'bool', reading the bit, or can be assigned to, to set the value of the bit.
    iterator begin() const
    {
        return iterator(const_cast<IntegerType*>(this->m_bits), 0);
    }

    iterator end() const
    {
        return iterator(const_cast<IntegerType*>(this->m_bits), this->m_size);
    }

    /// Returns a proxy to the bit at the given index. The proxy is the same as is returned by the iterators.
    Proxy operator[](std::size_t index) const
    {
        assert(index < this->m_size);
        return *std::next(begin(), index);
    }
};

} // namespace jllvm

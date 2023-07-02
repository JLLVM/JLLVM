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

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/Allocator.h>

namespace jllvm
{

/// Default hash and equality checked used by 'NonOwningFrozenSet'.
template <class T>
struct NonOwningFrozenSetDefaultInfo
{
    /// Method called to get a hash code from a value.
    /// It is templated to work on a larger variety of types allowing heterogeneous lookup.
    /// It is up to the caller to make sure two hash implementations of two types are equal.
    template <class U>
    static llvm::hash_code getHashCode(const U& value)
    {
        using namespace llvm;
        // Make the call do a ADL lookup, but fallback to the implementations in the llvm namespace otherwise.
        return hash_value(value);
    }

    /// Method called to compare two keys.
    /// It is templated to work on a larger variety of types allowing heterogeneous lookup.
    template <class U>
    static bool isEqual(const T& lhs, const U& rhs)
    {
        return lhs == rhs;
    }
};

/// Immutable hash set that does not take ownership of the keys.
/// The use case for this hash set is for a range of 'Key's that are constructed and allocated elsewhere, do not change
/// after construction, and require fast lookup.
/// Additionally, the hash set preserves insertion order, is a standard layout type and allows heterogeneous lookup.
/// The hash algorithm and equality check are controlled by the 'Info' template argument.
template <class Key, class Info = NonOwningFrozenSetDefaultInfo<Key>>
class NonOwningFrozenSet
{
    llvm::ArrayRef<std::size_t> m_indices;
    llvm::ArrayRef<Key> m_keys;

    constexpr static double LOAD_FACTOR = 0.75;
    constexpr static std::size_t EMPTY_INDEX = static_cast<std::size_t>(-1);

    template <class U>
    std::size_t getBucket(const U& key) const
    {
        std::size_t mask = m_indices.size() - 1;
        std::size_t bucket = Info::getHashCode(key) & mask;
        for (std::size_t i = 1;; i++)
        {
            if (m_indices[bucket] == EMPTY_INDEX)
            {
                return bucket;
            }

            if (Info::isEqual(m_keys[m_indices[bucket]], key))
            {
                return bucket;
            }

            bucket += i;
            bucket &= mask;
        }
        llvm_unreachable("Load factor should ensure termination");
    }

public:
    using iterator = decltype(m_keys)::iterator;

    NonOwningFrozenSet() = default;

    /// Constructs a new 'NonOwningFrozenSet' with the given keys. The lifetime of 'keys' is managed externally.
    /// 'allocator' is used for allocation of any internal data required to construct the set and must also outlive
    /// the set instance.
    ///
    /// If 'keys' has any duplicates, latter instances will not be inserted into the set and cannot be found by 'find'.
    /// They remain accessible through 'begin' and 'end' however.
    explicit NonOwningFrozenSet(llvm::ArrayRef<Key> keys, llvm::BumpPtrAllocator& allocator) : m_keys(keys)
    {
        if (keys.empty())
        {
            return;
        }

        std::size_t size = llvm::PowerOf2Ceil(keys.size());
        if (keys.size() / (double)size > LOAD_FACTOR)
        {
            size = llvm::NextPowerOf2(size);
        }

        llvm::MutableArrayRef<std::size_t> indices{allocator.Allocate<std::size_t>(size), size};
        std::fill_n(indices.begin(), size, EMPTY_INDEX);
        m_indices = indices;

        for (auto&& [index, k] : llvm::enumerate(keys))
        {
            std::size_t bucket = getBucket(k);
            if (indices[bucket] == EMPTY_INDEX)
            {
                indices[bucket] = index;
            }
        }
    }

    /// Returns an iterator to the element with they key 'key' or 'end()' if no such element exists.
    /// The key must not be an instance of 'Key', but can be any type for which the hash and equal implementations
    /// are compatible. It is up to the caller to ensure that is true.
    template <class U>
    iterator find(const U& key) const
    {
        if (empty())
        {
            return end();
        }

        std::size_t bucket = getBucket(key);
        if (m_indices[bucket] == EMPTY_INDEX)
        {
            return end();
        }
        return m_keys.begin() + m_indices[bucket];
    }

    /// Returns true if 'find' can find an element.
    template <class U>
    bool contains(const U& key) const
    {
        return find(key) != end();
    }

    /// Returns true if this set is empty.
    bool empty() const
    {
        return m_keys.empty();
    }

    llvm::ArrayRef<Key> keys() const
    {
        return m_keys;
    }

    /// Returns the begin iterator over the keys.
    /// The iteration order is the same as given in the constructor.
    iterator begin() const
    {
        return m_keys.begin();
    }

    /// Returns the end iterator over the keys.
    iterator end() const
    {
        return m_keys.end();
    }
};

template <class Range>
NonOwningFrozenSet(const Range&, auto&) -> NonOwningFrozenSet<typename Range::value_type>;

} // namespace jllvm

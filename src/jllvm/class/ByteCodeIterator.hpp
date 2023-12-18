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
#include <llvm/ADT/iterator.h>

#include <jllvm/support/Variant.hpp>

#include <cstdint>

#include <swl/variant.hpp>

namespace jllvm
{
/// All JVM OpCodes that exist in version 17 with their identifying byte values.
/// https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-6.html
enum class OpCodes : std::uint8_t
{
#define GENERATE_SELECTOR(name, base, body, parser, size, code) name = (code),
#define GENERATE_SELECTOR_END(name, base, body, parser, size, code) name = (code),
#include "ByteCode.def"
};

struct ByteCodeBase
{
    std::size_t offset{};
};

struct SingletonOp : ByteCodeBase
{
};

struct LocalIndexedOp : ByteCodeBase
{
    std::uint8_t index{};
};

struct PoolIndexedOp : ByteCodeBase
{
    std::uint16_t index{};
};

struct BranchOffsetOp : ByteCodeBase
{
    std::int32_t target{};
};

struct ArrayOp : ByteCodeBase
{
    enum class ArrayType : std::uint8_t
    {
        TBoolean = 4,
        TChar = 5,
        TFloat = 6,
        TDouble = 7,
        TByte = 8,
        TShort = 9,
        TInt = 10,
        TLong = 11
    };

    ArrayType atype{};
};

struct SwitchOp : ByteCodeBase
{
    std::vector<std::pair<std::int32_t, std::int32_t>> matchOffsetsPairs;
    std::int32_t defaultOffset;
};

#define GENERATE_SELECTOR(name, base, body, parser, size, code) struct name : base body;
#define GENERATE_SELECTOR_END(name, base, body, parser, size, code) struct name : base body;
#include "ByteCode.def"

using ByteCodeOp = swl::variant<
#define GENERATE_SELECTOR(name, base, body, parser, size, code) name,
#define GENERATE_SELECTOR_END(name, base, body, parser, size, code) name
#include "ByteCode.def"
    >;

class ByteCodeIterator : public llvm::iterator_facade_base<ByteCodeIterator, std::forward_iterator_tag, ByteCodeOp,
                                                           std::ptrdiff_t, ByteCodeOp, ByteCodeOp>
{
    const char* m_current = nullptr;
    std::size_t m_offset = 0;

    // Returns the size of the operation, including the identifying byte.
    std::size_t currentOpSize() const;

    ByteCodeOp currentOp() const;

public:
    ByteCodeIterator() = default;

    explicit ByteCodeIterator(const char* data, std::size_t offset = 0) : m_current{data + offset}, m_offset{offset} {}

    bool operator==(ByteCodeIterator rhs) const
    {
        return m_current == rhs.m_current;
    }

    ByteCodeIterator& operator++()
    {
        std::size_t size = currentOpSize();
        m_current += size;
        m_offset += size;
        return *this;
    }

    value_type operator*() const
    {
        return currentOp();
    }

    /// Returns the current bytecode offset the iterator is at.
    std::size_t getOffset()
    {
        return m_offset;
    }
};

/// Returns an iterator range returning a 'ByteCodeOp' for every JVM instruction starting from 'offset' inside 'data'.
/// Assumes that 'data' contains valid bytecode.
inline auto byteCodeRange(llvm::ArrayRef<char> data, std::uint16_t offset = 0)
{
    return llvm::make_range(ByteCodeIterator(data.begin(), offset), ByteCodeIterator(data.end()));
}

inline std::size_t getOffset(const ByteCodeOp& op)
{
    return match(op, [](const auto& op) { return op.offset; });
}

namespace detail
{
template <class First, class... Rest>
struct MechanismForBase
{
    using Base = std::conditional_t<
        std::is_base_of_v<SingletonOp, First>, SingletonOp,
        std::conditional_t<std::is_base_of_v<LocalIndexedOp, First>, LocalIndexedOp,
                           std::conditional_t<std::is_base_of_v<PoolIndexedOp, First>, PoolIndexedOp,
                                              std::conditional_t<std::is_base_of_v<BranchOffsetOp, First>,
                                                                 BranchOffsetOp, SwitchOp>>>>;

    static_assert((std::is_base_of_v<Base, Rest> && ...));
};
} // namespace detail

template <class... Args>
struct OneOf : detail::MechanismForBase<Args...>::Base
{
    using Base = typename detail::MechanismForBase<Args...>::Base;

    template <class T, std::enable_if_t<(std::is_same_v<std::decay_t<T>, Args> || ...)>* = nullptr>
    OneOf(T&& value) : Base(std::forward<T>(value))
    {
    }
};

/// Satisfied when 'T' is an add operation.
template <class T>
concept IsAdd = llvm::is_one_of<T, IAdd, FAdd, DAdd, LAdd>::value;

/// Satisfied when 'T' is a sub operation.
template <class T>
concept IsSub = llvm::is_one_of<T, ISub, FSub, DSub, LSub>::value;

/// Satisfied when 'T' is a neg operation.
template <class T>
concept IsNeg = llvm::is_one_of<T, INeg, FNeg, DNeg, LNeg>::value;

/// Satisfied when 'T' is a mul operation.
template <class T>
concept IsMul = llvm::is_one_of<T, IMul, FMul, DMul, LMul>::value;

/// Satisfied when 'T' is a load operation.
template <class T>
concept IsLoad = llvm::is_one_of<T, ILoad, ALoad, FLoad, DLoad, LLoad>::value;

/// Satisfied when 'T' is a load0 operation.
template <class T>
concept IsLoad0 = llvm::is_one_of<T, ILoad0, ALoad0, FLoad0, DLoad0, LLoad0>::value;

/// Satisfied when 'T' is a load1 operation.
template <class T>
concept IsLoad1 = llvm::is_one_of<T, ILoad1, ALoad1, FLoad1, DLoad1, LLoad1>::value;

/// Satisfied when 'T' is a load2 operation.
template <class T>
concept IsLoad2 = llvm::is_one_of<T, ILoad2, ALoad2, FLoad2, DLoad2, LLoad2>::value;

/// Satisfied when 'T' is a load3 operation.
template <class T>
concept IsLoad3 = llvm::is_one_of<T, ILoad3, ALoad3, FLoad3, DLoad3, LLoad3>::value;

/// Satisfied when 'T' is a store operation.
template <class T>
concept IsStore = llvm::is_one_of<T, IStore, AStore, FStore, DStore, LStore>::value;

/// Satisfied when 'T' is a store0 operation.
template <class T>
concept IsStore0 = llvm::is_one_of<T, IStore0, AStore0, FStore0, DStore0, LStore0>::value;

/// Satisfied when 'T' is a store1 operation.
template <class T>
concept IsStore1 = llvm::is_one_of<T, IStore1, AStore1, FStore1, DStore1, LStore1>::value;

/// Satisfied when 'T' is a store2 operation.
template <class T>
concept IsStore2 = llvm::is_one_of<T, IStore2, AStore2, FStore2, DStore2, LStore2>::value;

/// Satisfied when 'T' is a store3 operation.
template <class T>
concept IsStore3 = llvm::is_one_of<T, IStore3, AStore3, FStore3, DStore3, LStore3>::value;

/// Satisfied when 'T' operates on 'int' operands.
template <class T>
concept OperatesOnIntegers = llvm::is_one_of<T, ILoad, ILoad0, ILoad1, ILoad2, ILoad3, IStore, IStore0, IStore1,
                                             IStore2, IStore3, IAdd, ISub, IMul, IDiv, IRem, IInc, INeg>::value;

/// Satisfied when 'T' operates on reference operands.
template <class T>
concept OperatesOnReferences =
    llvm::is_one_of<T, ALoad, ALoad0, ALoad1, ALoad2, ALoad3, AStore, AStore0, AStore1, AStore2, AStore3>::value;

/// Satisfied when 'T' operates on 'float' operands.
template <class T>
concept OperatesOnFloat = llvm::is_one_of<T, FLoad, FLoad0, FLoad1, FLoad2, FLoad3, FStore, FStore0, FStore1, FStore2,
                                          FStore3, FAdd, FSub, FMul, FDiv, FRem, FNeg>::value;

/// Satisfied when 'T' operates on 'double' operands.
template <class T>
concept OperatesOnDouble = llvm::is_one_of<T, DLoad, DLoad0, DLoad1, DLoad2, DLoad3, DStore, DStore0, DStore1, DStore2,
                                           DStore3, DAdd, DSub, DMul, DDiv, DRem, DNeg>::value;

/// Satisfied when 'T' operates on 'long' operands.
template <class T>
concept OperatesOnLong = llvm::is_one_of<T, LLoad, LLoad0, LLoad1, LLoad2, LLoad3, LStore, LStore0, LStore1, LStore2,
                                         LStore3, LAdd, LSub, LMul, LDiv, LRem, LNeg>::value;

} // namespace jllvm

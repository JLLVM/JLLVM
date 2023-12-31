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

#include "ByteCodeIterator.hpp"

#include <llvm/Support/Alignment.h>
#include <llvm/Support/Endian.h>

#include <jllvm/support/Bytes.hpp>

namespace
{
using namespace jllvm;

template <class Singleton>
ByteCodeOp parseSingleton(const char*, std::size_t offset)
{
    return Singleton{offset};
}

template <class LocalIndexed>
ByteCodeOp parseLocalIndexed(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return LocalIndexed{offset, consume<std::uint8_t>(bytes)};
}

template <class PoolIndexed>
ByteCodeOp parsePoolIndexed(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    if constexpr (std::is_same_v<PoolIndexed, LDC>)
    {
        return LDC{offset, consume<std::uint8_t>(bytes)};
    }
    else if constexpr (std::is_same_v<PoolIndexed, InvokeDynamic>)
    {
        auto index = consume<std::uint16_t>(bytes);
        auto padding = consume<std::uint16_t>(bytes);
        assert(padding == 0);
        return InvokeDynamic{offset, index};
    }
    else if constexpr (std::is_same_v<PoolIndexed, InvokeInterface>)
    {
        auto index = consume<std::uint16_t>(bytes);
        auto count = consume<std::uint8_t>(bytes);
        auto padding = consume<std::uint8_t>(bytes);
        assert(count != 0);
        assert(padding == 0);
        return InvokeInterface{offset, index};
    }
    else
    {
        return PoolIndexed{offset, consume<std::uint16_t>(bytes)};
    }
}

template <class BranchOffset>
ByteCodeOp parseBranchOffset(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    if constexpr (std::is_same_v<BranchOffset, GotoW> || std::is_same_v<BranchOffset, JSRw>)
    {
        return BranchOffset{offset, consume<std::int32_t>(bytes)};
    }
    else
    {
        return BranchOffset{offset, consume<std::int16_t>(bytes)};
    }
}

template <std::same_as<BIPush>>
ByteCodeOp parseBIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return BIPush{offset, consume<std::int8_t>(bytes)};
}

template <std::same_as<NewArray>>
ByteCodeOp parseNewArray(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return NewArray{offset, consume<BaseType::Values>(bytes)};
}

template <std::same_as<IInc>>
ByteCodeOp parseIInc(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return IInc{offset, consume<std::uint8_t>(bytes), consume<std::int8_t>(bytes)};
}

template <std::same_as<SIPush>>
ByteCodeOp parseSIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return SIPush{offset, consume<std::int16_t>(bytes)};
}

template <std::same_as<MultiANewArray>>
ByteCodeOp parseMultiANewArray(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    auto index = consume<std::uint16_t>(bytes);
    auto dimensions = consume<std::uint8_t>(bytes);
    assert(dimensions >= 1);
    return MultiANewArray{offset, index, dimensions};
}

template <std::same_as<LookupSwitch>>
ByteCodeOp parseLookupSwitch(const char* bytes, std::size_t offset)
{
    bytes += 4 - (offset % 4);

    auto defaultOffset = consume<std::int32_t>(bytes);
    auto pairCount = consume<std::int32_t>(bytes);
    assert(pairCount >= 0);

    return LookupSwitch{offset, defaultOffset, BigEndianArrayRef<std::uint64_t>(bytes, pairCount)};
}

template <std::same_as<TableSwitch>>
ByteCodeOp parseTableSwitch(const char* bytes, std::size_t offset)
{
    bytes += 4 - (offset % 4);

    auto defaultOffset = consume<std::int32_t>(bytes);
    auto lowByte = consume<std::int32_t>(bytes);
    auto highByte = consume<std::int32_t>(bytes);
    assert(lowByte <= highByte);
    return TableSwitch{offset, defaultOffset, lowByte, BigEndianArrayRef<std::int32_t>(bytes, highByte - lowByte + 1)};
}

template <std::same_as<Wide>>
ByteCodeOp parseWide(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    auto opCode = consume<OpCodes>(bytes);
    auto index = consume<std::uint16_t>(bytes);
    std::optional<std::int16_t> value{};
    if (opCode == OpCodes::IInc)
    {
        value = consume<std::int16_t>(bytes);
    }

    return Wide{offset, opCode, index, value};
}

std::size_t lookupSwitchSize(const char* bytes, std::size_t offset)
{
    std::uint64_t padding = 3 - (offset % 4);
    const char* pairCountPtr = bytes + 1 + padding + 4;
    auto pairCount = consume<std::int32_t>(pairCountPtr);

    return 1 + padding + 4 + 4 + 8 * pairCount;
}

std::size_t tableSwitchSize(const char* bytes, std::size_t offset)
{
    std::uint64_t padding = 3 - (offset % 4);
    const char* padded = bytes + 1 + padding + 4;
    auto lowByte = consume<std::int32_t>(padded);
    auto highByte = consume<std::int32_t>(padded);
    return 1 + padding + 4 + 4 + 4 + (highByte - lowByte + 1) * 4;
}

std::size_t wideSize(const char* bytes)
{
    return static_cast<OpCodes>(bytes[1]) == OpCodes::IInc ? 6 : 4;
}
} // namespace

jllvm::ByteCodeOp jllvm::ByteCodeIterator::currentOp() const
{
    switch (static_cast<OpCodes>(*m_current))
    {
#define GENERATE_SELECTOR(name, base, body, parser, size, code) \
    case OpCodes::name:                                         \
        return parser<name>(m_current, m_offset); // NOLINT(bugprone-macro-parentheses) parser is always a function
#define GENERATE_SELECTOR_END(name, base, body, parser, size, code) \
    case OpCodes::name:                                             \
        return parser<name>(m_current, m_offset); // NOLINT(bugprone-macro-parentheses) parser is always a function
#include "ByteCode.def"
        default: llvm_unreachable("Unknown opcode");
    }
}

std::size_t jllvm::ByteCodeIterator::currentOpSize() const
{
    switch (static_cast<OpCodes>(*m_current))
    {
#define GENERATE_SELECTOR(name, base, body, parser, size, code) \
    case OpCodes::name: return (size);
#define GENERATE_SELECTOR_END(name, base, body, parser, size, code) \
    case OpCodes::name: return (size);
#include "ByteCode.def"
        default: llvm_unreachable("Unknown opcode");
    }
}

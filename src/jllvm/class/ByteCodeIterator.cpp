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

template <std::same_as<BIPush> OpCode>
ByteCodeOp parseBIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return BIPush{offset, consume<std::int8_t>(bytes)};
}

template <std::same_as<NewArray> OpCode>
ByteCodeOp parseNewArray(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return NewArray{offset, consume<ArrayOp::ArrayType>(bytes)};
}

template <std::same_as<IInc> OpCode>
ByteCodeOp parseIInc(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return IInc{offset, consume<std::uint8_t>(bytes), consume<std::int8_t>(bytes)};
}

template <std::same_as<SIPush> OpCode>
ByteCodeOp parseSIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return SIPush{offset, consume<std::int16_t>(bytes)};
}

template <std::same_as<MultiANewArray> OpCode>
ByteCodeOp parseMultiANewArray(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    auto index = consume<std::uint16_t>(bytes);
    auto dimensions = consume<std::uint8_t>(bytes);
    assert(dimensions >= 1);
    return MultiANewArray{offset, index, dimensions};
}

template <std::same_as<LookupSwitch> OpCode>
ByteCodeOp parseLookupSwitch(const char* bytes, std::size_t offset)
{
    bytes += 4 - (offset % 4);

    auto defaultOffset = consume<std::int32_t>(bytes);
    auto pairCount = consume<std::int32_t>(bytes);
    assert(pairCount >= 0);

    std::vector<std::pair<std::int32_t, std::int32_t>> matchOffsetsPairs(pairCount);

    std::generate(matchOffsetsPairs.begin(), matchOffsetsPairs.end(),
                  [&] {
                      return std::pair{consume<std::int32_t>(bytes), consume<std::int32_t>(bytes)};
                  });

    return LookupSwitch{offset, std::move(matchOffsetsPairs), defaultOffset};
}

template <std::same_as<TableSwitch> OpCode>
ByteCodeOp parseTableSwitch(const char* bytes, std::size_t offset)
{
    bytes += 4 - (offset % 4);

    auto defaultOffset = consume<std::int32_t>(bytes);
    auto lowByte = consume<std::int32_t>(bytes);
    auto highByte = consume<std::int32_t>(bytes);

    assert(lowByte <= highByte);

    std::vector<std::pair<std::int32_t, std::int32_t>> matchOffsetsPairs(highByte - lowByte + 1);

    std::generate(matchOffsetsPairs.begin(), matchOffsetsPairs.end(),
                  [&] {
                      return std::pair{lowByte++, consume<std::int32_t>(bytes)};
                  });

    return TableSwitch{offset, std::move(matchOffsetsPairs), defaultOffset};
}

template <class>
ByteCodeOp parseNotImplemented(const char*, std::size_t)
{
    llvm::report_fatal_error("NOT YET IMPLEMENTED");
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

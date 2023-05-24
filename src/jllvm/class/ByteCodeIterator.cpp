#include "ByteCodeIterator.hpp"

#include <llvm/Support/Alignment.h>
#include <llvm/Support/Endian.h>

#include <jllvm/support/Bytes.hpp>

#include "jllvm/object/Object.hpp"

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

template <class OpCode>
requires(std::is_same_v<OpCode, BIPush>) ByteCodeOp parseBIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return BIPush{offset, consume<std::int8_t>(bytes)};
}

template <class OpCode>
requires(std::is_same_v<OpCode, NewArray>) ByteCodeOp parseNewArray(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return NewArray{offset, consume<ArrayOp::ArrayType>(bytes)};
}

template <class OpCode>
requires(std::is_same_v<OpCode, IInc>) ByteCodeOp parseIInc(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return IInc{offset, consume<std::uint8_t>(bytes), consume<std::int8_t>(bytes)};
}

template <class OpCode>
requires(std::is_same_v<OpCode, SIPush>) ByteCodeOp parseSIPush(const char* bytes, std::size_t offset)
{
    consume<OpCodes>(bytes);
    return SIPush{offset, consume<std::int16_t>(bytes)};
}

template <class OpCode>
ByteCodeOp parseNotImplemented(const char*, std::size_t)
{
    llvm::report_fatal_error("NOT YET IMPLEMENTED");
}

std::size_t lookupSwitchSize(const char* bytes)
{
    std::uint64_t padding = llvm::offsetToAlignedAddr(bytes + 1, llvm::Align(4));
    const char* pairCountPtr = bytes + 5 + padding;
    std::uint32_t pairCount;
    std::memcpy(&pairCount, pairCountPtr, sizeof(std::uint32_t));
    pairCount = llvm::support::endian::byte_swap(pairCount, llvm::support::big);
    return 1 + padding + 4 + 8 * pairCount;
}

std::size_t tableSwitchSize(const char* bytes)
{
    std::uint64_t padding = llvm::offsetToAlignedAddr(bytes + 1, llvm::Align(4));
    const char* padded = bytes + 5 + padding;
    std::uint32_t lowByte;
    std::memcpy(&lowByte, padded, sizeof(std::uint32_t));
    lowByte = llvm::support::endian::byte_swap(lowByte, llvm::support::big);
    padded += sizeof(std::uint32_t);
    std::uint32_t highByte;
    std::memcpy(&highByte, padded, sizeof(std::uint32_t));
    highByte = llvm::support::endian::byte_swap(highByte, llvm::support::big);

    return 1 + padding + 4 + 4 + 4 + (highByte - lowByte + 1) * 4;
}

std::size_t wideSize(const char* bytes)
{
    return static_cast<OpCodes>(bytes[1]) == OpCodes::IInc ? 6 : 4;
}
} // namespace

jllvm::ArrayOp::ArrayInfo jllvm::ArrayOp::resolve(llvm::IRBuilder<>& builder)
{
    switch (atype)
    {
        case ArrayType::TBoolean:
            return {"Z", builder.getInt8Ty(), sizeof(std::uint8_t), jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayType::TChar:
            return {"C", builder.getInt16Ty(), sizeof(std::uint16_t),
                    jllvm::Array<std::uint16_t>::arrayElementsOffset()};
        case ArrayType::TFloat:
            return {"F", builder.getFloatTy(), sizeof(float), jllvm::Array<float>::arrayElementsOffset()};
        case ArrayType::TDouble:
            return {"D", builder.getDoubleTy(), sizeof(double), jllvm::Array<double>::arrayElementsOffset()};
        case ArrayType::TByte:
            return {"B", builder.getInt8Ty(), sizeof(std::uint8_t), jllvm::Array<std::uint8_t>::arrayElementsOffset()};
        case ArrayType::TShort:
            return {"S", builder.getInt16Ty(), sizeof(std::int16_t), jllvm::Array<std::int16_t>::arrayElementsOffset()};
        case ArrayType::TInt:
            return {"I", builder.getInt32Ty(), sizeof(std::int32_t), jllvm::Array<std::int32_t>::arrayElementsOffset()};
        case ArrayType::TLong:
            return {"J", builder.getInt64Ty(), sizeof(std::int64_t), jllvm::Array<std::int64_t>::arrayElementsOffset()};
        default: llvm_unreachable("Invalid array type");
    }
}

jllvm::ByteCodeOp jllvm::ByteCodeIterator::currentOp() const
{
    switch (static_cast<OpCodes>(*m_current))
    {
#define GENERATE_SELECTOR(name, base, body, parser, size) \
    case OpCodes::name:                                   \
        return parser<name>(m_current, m_offset); // NOLINT(bugprone-macro-parentheses) parser is always a function
#define GENERATE_SELECTOR_END(name, base, body, parser, size) \
    case OpCodes::name:                                       \
        return parser<name>(m_current, m_offset); // NOLINT(bugprone-macro-parentheses) parser is always a function
#include "ByteCode.def"
        default: llvm_unreachable("Unknown opcode");
    }
}

std::size_t jllvm::ByteCodeIterator::currentOpSize() const
{
    switch (static_cast<OpCodes>(*m_current))
    {
#define GENERATE_SELECTOR(name, base, body, parser, size) \
    case OpCodes::name: return (size);
#define GENERATE_SELECTOR_END(name, base, body, parser, size) \
    case OpCodes::name: return (size);
#include "ByteCode.def"
        default: llvm_unreachable("Unknown opcode");
    }
}

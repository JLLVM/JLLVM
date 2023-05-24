#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/iterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <jllvm/support/Variant.hpp>

#include <cstdint>

#include <swl/variant.hpp>

namespace jllvm
{
/// All JVM OpCodes that exist in version 17 with their identifying byte values.
/// https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-6.html
enum class OpCodes : std::uint8_t
{
    AALoad = 0x32,
    AAStore = 0x53,
    AConstNull = 0x1,
    ALoad = 0x19,
    ALoad0 = 0x2a,
    ALoad1 = 0x2b,
    ALoad2 = 0x2c,
    ALoad3 = 0x2d,
    ANewArray = 0xbd,
    AReturn = 0xb0,
    ArrayLength = 0xbe,
    AStore = 0x3a,
    AStore0 = 0x4b,
    AStore1 = 0x4c,
    AStore2 = 0x4d,
    AStore3 = 0x4e,
    AThrow = 0xbf,
    BALoad = 0x33,
    BAStore = 0x54,
    BIPush = 0x10,
    CALoad = 0x34,
    CAStore = 0x55,
    CheckCast = 0xc0,
    D2F = 0x90,
    D2I = 0x8e,
    D2L = 0x8f,
    DAdd = 0x63,
    DALoad = 0x31,
    DAStore = 0x52,
    DCmpG = 0x98,
    DCmpL = 0x97,
    DConst0 = 0xe,
    DConst1 = 0xf,
    DDiv = 0x6f,
    DLoad = 0x18,
    DLoad0 = 0x26,
    DLoad1 = 0x27,
    DLoad2 = 0x28,
    DLoad3 = 0x29,
    DMul = 0x6b,
    DNeg = 0x77,
    DRem = 0x73,
    DReturn = 0xaf,
    DStore = 0x39,
    DStore0 = 0x47,
    DStore1 = 0x48,
    DStore2 = 0x49,
    DStore3 = 0x4a,
    DSub = 0x67,
    Dup = 0x59,
    DupX1 = 0x5a,
    DupX2 = 0x5b,
    Dup2 = 0x5c,
    Dup2X1 = 0x5d,
    Dup2X2 = 0x5e,
    F2D = 0x8d,
    F2I = 0x8b,
    F2L = 0x8c,
    FAdd = 0x62,
    FALoad = 0x30,
    FAStore = 0x51,
    FCmpG = 0x96,
    FCmpL = 0x95,
    FConst0 = 0xb,
    FConst1 = 0xc,
    FConst2 = 0xd,
    FDiv = 0x6e,
    FLoad = 0x17,
    FLoad0 = 0x22,
    FLoad1 = 0x23,
    FLoad2 = 0x24,
    FLoad3 = 0x25,
    FMul = 0x6a,
    FNeg = 0x76,
    FRem = 0x72,
    FReturn = 0xae,
    FStore = 0x38,
    FStore0 = 0x43,
    FStore1 = 0x44,
    FStore2 = 0x45,
    FStore3 = 0x46,
    FSub = 0x66,
    GetField = 0xb4,
    GetStatic = 0xb2,
    Goto = 0xa7,
    GotoW = 0xc8,
    I2B = 0x91,
    I2C = 0x92,
    I2D = 0x87,
    I2F = 0x86,
    I2L = 0x85,
    I2S = 0x93,
    IAdd = 0x60,
    IALoad = 0x2e,
    IAnd = 0x7e,
    IAStore = 0x4f,
    IConstM1 = 0x2,
    IConst0 = 0x3,
    IConst1 = 0x4,
    IConst2 = 0x5,
    IConst3 = 0x6,
    IConst4 = 0x7,
    IConst5 = 0x8,
    IDiv = 0x6c,
    IfACmpEq = 0xa5,
    IfACmpNe = 0xa6,
    IfICmpEq = 0x9f,
    IfICmpNe = 0xa0,
    IfICmpLt = 0xa1,
    IfICmpGe = 0xa2,
    IfICmpGt = 0xa3,
    IfICmpLe = 0xa4,
    IfEq = 0x99,
    IfNe = 0x9a,
    IfLt = 0x9b,
    IfGe = 0x9c,
    IfGt = 0x9d,
    IfLe = 0x9e,
    IfNonNull = 0xc7,
    IfNull = 0xc6,
    IInc = 0x84,
    ILoad = 0x15,
    ILoad0 = 0x1a,
    ILoad1 = 0x1b,
    ILoad2 = 0x1c,
    ILoad3 = 0x1d,
    IMul = 0x68,
    INeg = 0x74,
    InstanceOf = 0xc1,
    InvokeDynamic = 0xba,
    InvokeInterface = 0xb9,
    InvokeSpecial = 0xb7,
    InvokeStatic = 0xb8,
    InvokeVirtual = 0xb6,
    IOr = 0x80,
    IRem = 0x70,
    IReturn = 0xac,
    IShl = 0x78,
    IShr = 0x7a,
    IStore = 0x36,
    IStore0 = 0x3b,
    IStore1 = 0x3c,
    IStore2 = 0x3d,
    IStore3 = 0x3e,
    ISub = 0x64,
    IUShr = 0x7c,
    IXor = 0x82,
    JSR = 0xa8,
    JSRw = 0xc9,
    L2D = 0x8a,
    L2F = 0x89,
    L2I = 0x88,
    LAdd = 0x61,
    LALoad = 0x2f,
    LAnd = 0x7f,
    LAStore = 0x50,
    LCmp = 0x94,
    LConst0 = 0x9,
    LConst1 = 0xa,
    LDC = 0x12,
    LDCW = 0x13,
    LDC2W = 0x14,
    LDiv = 0x6d,
    LLoad = 0x16,
    LLoad0 = 0x1e,
    LLoad1 = 0x1f,
    LLoad2 = 0x20,
    LLoad3 = 0x21,
    LMul = 0x69,
    LNeg = 0x75,
    LookupSwitch = 0xab,
    LOr = 0x81,
    LRem = 0x71,
    LReturn = 0xad,
    LShl = 0x79,
    LShr = 0x7b,
    LStore = 0x37,
    LStore0 = 0x3f,
    LStore1 = 0x40,
    LStore2 = 0x41,
    LStore3 = 0x42,
    LSub = 0x65,
    LUShr = 0x7d,
    LXor = 0x83,
    MonitorEnter = 0xc2,
    MonitorExit = 0xc3,
    MultiANewArray = 0xc5,
    New = 0xbb,
    NewArray = 0xbc,
    Nop = 0x0,
    Pop = 0x57,
    Pop2 = 0x58,
    PutField = 0xb5,
    PutStatic = 0xb3,
    Ret = 0xa9,
    Return = 0xb1,
    SALoad = 0x35,
    SAStore = 0x56,
    SIPush = 0x11,
    Swap = 0x5f,
    TableSwitch = 0xaa,
    Wide = 0xc4,
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

    std::tuple<llvm::StringRef, std::size_t, llvm::Type*> resolve(llvm::IRBuilder<>&);

    ArrayType atype{};
};

#define GENERATE_SELECTOR(name, base, body, parser, size) struct name : base body;
#define GENERATE_SELECTOR_END(name, base, body, parser, size) struct name : base body;
#include "ByteCode.def"

using ByteCodeOp = swl::variant<
#define GENERATE_SELECTOR(name, base, body, parser, size) name,
#define GENERATE_SELECTOR_END(name, base, body, parser, size) name
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

    explicit ByteCodeIterator(const char* current) : m_current(current) {}

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
};

/// Returns an iterator range returning a 'ByteCodeOp' for every JVM instruction.
/// Assumes that 'current' contains valid byte code.
inline auto byteCodeRange(llvm::ArrayRef<char> current)
{
    return llvm::make_range(ByteCodeIterator(current.begin()), ByteCodeIterator(current.end()));
}

inline auto getOffset(const ByteCodeOp& op)
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
                           std::conditional_t<std::is_base_of_v<PoolIndexedOp, First>, PoolIndexedOp, BranchOffsetOp>>>;

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

} // namespace jllvm

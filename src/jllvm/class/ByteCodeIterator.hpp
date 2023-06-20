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

    explicit ByteCodeIterator(const char* current, std::size_t offset = 0) : m_current{current}, m_offset{offset} {}

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
inline auto byteCodeRange(llvm::ArrayRef<char> current, std::uint16_t offset = 0)
{
    return llvm::make_range(ByteCodeIterator(current.begin(), offset), ByteCodeIterator(current.end()));
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

} // namespace jllvm

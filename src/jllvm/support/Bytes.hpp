
#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Endian.h>

#include <cassert>

namespace jllvm
{

/// Reads a instance of 'T' from 'bytes', advancing 'bytes' by the amount of bytes read.
/// 'T' must be a trivially copyable type for this to be legal.
/// Note: This function is mainly used to support parsing JVM formats which are all big endian.
/// This function therefore also does conversion from big endian to the host format.
/// Asserts if 'bytes' does not contain enough bytes.
template <class T>
T consume(llvm::ArrayRef<char>& bytes)
{
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 8, "T must be trivially copyable");
    assert(bytes.size() >= sizeof(T));
    std::conditional_t<sizeof(T) <= 2, std::conditional_t<sizeof(T) <= 1, std::uint8_t, std::uint16_t>,
                       std::conditional_t<(sizeof(T) > 4), std::uint64_t, std::uint32_t>>
        result;
    std::memcpy(&result, bytes.data(), sizeof(T));
    bytes = bytes.drop_front(sizeof(T));
    result = llvm::support::endian::byte_swap(result, llvm::support::big);
    return T(result);
}

/// Reads in 'length' amount of bytes from 'bytes', returns it as a 'StringRef' and advanced 'bytes' by the amount of
/// bytes read.
/// Asserts if 'bytes' does not contain enough bytes.
inline llvm::StringRef consumeRawString(std::size_t length, llvm::ArrayRef<char>& bytes)
{
    assert(bytes.size() >= length);
    auto result = llvm::StringRef(bytes.data(), length);
    bytes = bytes.drop_front(length);
    return result;
}

} // namespace jllvm

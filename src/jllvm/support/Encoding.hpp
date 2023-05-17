#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <vector>

namespace jllvm
{
/// Kinds of encoding used internally by Java strings.
enum class CompactEncoding : uint8_t
{
    /// LATIN1 encoding. Subset of unicode which encompasses all codepoints from 0 to 0xFF.
    Latin1 = 0,
    /// Big Endian UTF16 without a BOM.
    Utf16BE = 1,
};

/// Converts a utf8 string, as we use internally the majority of the time, to a Java Strings internal encoding.
/// Java strings use both Latin1 and UTF-16 depending on the content of the string. To save on memory,
/// the former is always used if it is capable of representing all values of the string. Otherwise
/// it switches to UTF-16 stored in big endian.
/// This method does exactly that conversion, returning the raw bytes of the converted output and the encoding used.
std::pair<std::vector<std::uint8_t>, CompactEncoding> toJavaCompactEncoding(llvm::StringRef utf8String);

std::string fromJavaCompactEncoding(std::pair<llvm::ArrayRef<std::uint8_t>, CompactEncoding> compactEncoding);

} // namespace jllvm

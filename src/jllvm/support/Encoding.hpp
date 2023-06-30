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
#include <llvm/ADT/StringRef.h>

#include <vector>

namespace jllvm
{
/// Kinds of encoding used internally by Java strings.
enum class CompactEncoding : uint8_t
{
    /// LATIN1 encoding. Subset of unicode which encompasses all codepoints from 0 to 0xFF.
    Latin1 = 0,
    /// UTF16 in native byte order without a BOM.
    Utf16 = 1,
};

/// Converts a utf8 string, as we use internally the majority of the time, to a Java Strings internal encoding.
/// Java strings use both Latin1 and UTF-16 depending on the content of the string. To save on memory,
/// the former is always used if it is capable of representing all values of the string. Otherwise
/// it switches to UTF-16 stored in big endian.
/// This method does exactly that conversion, returning the raw bytes of the converted output and the encoding used.
std::pair<std::vector<std::uint8_t>, CompactEncoding> toJavaCompactEncoding(llvm::StringRef utf8String);

std::string fromJavaCompactEncoding(std::pair<llvm::ArrayRef<std::uint8_t>, CompactEncoding> compactEncoding);

} // namespace jllvm

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
#include <llvm/Support/Endian.h>

#include <cassert>

namespace jllvm
{

/// Smallest unsigned integer type that is at least as large as 'T'.
template <class T>
requires(sizeof(T) <= sizeof(std::uint64_t)) using NextSizedUInt =
    std::conditional_t<sizeof(T) <= 2, std::conditional_t<sizeof(T) <= 1, std::uint8_t, std::uint16_t>,
                       std::conditional_t<(sizeof(T) > 4), std::uint64_t, std::uint32_t>>;

/// Reads an instance of 'T' from 'bytes', advancing 'bytes' by the amount of bytes read.
/// 'T' must be a trivially copyable type for this to be legal.
/// Note: This function is mainly used to support parsing JVM formats which are all big endian.
/// This function therefore also does conversion from big endian to the host format.
/// Asserts if 'bytes' does not contain enough bytes.
template <class T>
T consume(llvm::ArrayRef<char>& bytes)
{
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    assert(bytes.size() >= sizeof(T));
    NextSizedUInt<T> asBytes;
    std::memcpy(&asBytes, bytes.data(), sizeof(T));
    bytes = bytes.drop_front(sizeof(T));
    asBytes = llvm::support::endian::byte_swap(asBytes, llvm::support::big);

    T result;
    std::memcpy(&result, &asBytes, sizeof(T));
    return result;
}

template <class T>
T consume(const char*& bytes)
{
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    NextSizedUInt<T> asBytes;
    std::memcpy(&asBytes, bytes, sizeof(T));
    bytes += sizeof(T);
    asBytes = llvm::support::endian::byte_swap(asBytes, llvm::support::big);

    T result;
    std::memcpy(&result, &asBytes, sizeof(T));
    return result;
}

/// Reads in 'length' amount of bytes from 'bytes', returns it as a 'StringRef' and advanced 'bytes' by the amount of
/// bytes read.
/// Asserts if 'bytes' does not contain enough bytes.
llvm::StringRef consumeRawString(std::size_t length, llvm::ArrayRef<char>& bytes);

} // namespace jllvm

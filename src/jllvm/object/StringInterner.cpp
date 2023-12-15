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

#include "StringInterner.hpp"

void jllvm::StringInterner::checkStructure()
{
#ifdef NDEBUG
    for (const auto& item : m_stringClass->getFields())
    {
        if (item.isStatic())
        {
            continue;
        }
        bool valid;
        if (item.getName() == "value")
        {
            valid = item.getOffset() == 16 && item.getType() == byteArrayDescriptor;
        }
        else if (item.getName() == "coder")
        {
            valid = item.getOffset() == 24 && item.getType() == "B";
        }
        else if (item.getName() == "hash")
        {
            valid = item.getOffset() == 28 && item.getType() == "I";
        }
        else if (item.getName() == "hashIsZero")
        {
            valid = item.getOffset() == 32 && item.getType() == "Z";
        }
        else
        {
            llvm::report_fatal_error("Unexpected field in java.lang.String: " + item.getName());
        }
        assert(valid);
    }
#endif
}

jllvm::String* jllvm::StringInterner::createString(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding)
{
    assert(m_stringClass && "String class object must be initialized");

    auto* value = Array<std::uint8_t>::create(m_allocator, m_byteArrayClass, buffer.size());
    llvm::copy(buffer, value->begin());

    auto* string = new (m_allocator.Allocate(sizeof(String), alignof(String)))
        String(m_stringClass, value, static_cast<std::uint8_t>(encoding));

    m_contentToStringMap.insert({{{value->begin(), value->end()}, static_cast<std::uint8_t>(encoding)}, string});

    return string;
}

jllvm::String* jllvm::StringInterner::intern(llvm::StringRef utf8String)
{
    auto [buffer, encoding] = toJavaCompactEncoding(utf8String);
    return intern(buffer, encoding);
}

jllvm::String* jllvm::StringInterner::intern(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding)
{
    auto it = m_contentToStringMap.find({buffer, static_cast<uint8_t>(encoding)});
    if (it != m_contentToStringMap.end())
    {
        return it->second;
    }
    return createString(buffer, encoding);
}

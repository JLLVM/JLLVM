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

#include <llvm/Support/raw_ostream.h>

#include <jllvm/support/Encoding.hpp>

#include "ClassLoader.hpp"

namespace jllvm
{
class StringInterner
{
    static constexpr FieldType stringDescriptor = "Ljava/lang/String;";
    static constexpr FieldType byteArrayDescriptor = "[B";

    llvm::DenseMap<std::pair<llvm::ArrayRef<std::uint8_t>, std::uint8_t>, String*> m_contentToStringMap;
    llvm::BumpPtrAllocator m_allocator;
    ClassLoader& m_classLoader;
    ClassObject* m_stringClass{nullptr};

    void checkStructure();

    String* createString(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);

public:
    StringInterner(ClassLoader& classLoader) : m_classLoader(classLoader) {}

    void loadStringClass();

    ClassObject& getStringClass() const
    {
        return *m_stringClass;
    }

    String* intern(llvm::StringRef utf8String);

    String* intern(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);
};
} // namespace jllvm

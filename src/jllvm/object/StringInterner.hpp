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

#include "ClassObject.hpp"

namespace jllvm
{
class StringInterner
{
    static constexpr FieldType stringDescriptor = "Ljava/lang/String;";
    static constexpr FieldType byteArrayDescriptor = "[B";

    llvm::DenseMap<std::pair<llvm::ArrayRef<std::uint8_t>, std::uint8_t>, String*> m_contentToStringMap;
    llvm::BumpPtrAllocator m_allocator;
    ClassObject* m_byteArrayClass{};
    ClassObject* m_stringClass{};

    void checkStructure();

    String* createString(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);

public:
    /// Initialize the interner by loading the required Java classes. Has to be called before the first call to 'intern'.
    /// 'initializer' must return a pointer to a fully initialized class object for 'FieldType' of its argument.
    template <std::invocable<FieldType> F>
    void initialize(F&& initializer)
    {
        m_byteArrayClass = initializer(byteArrayDescriptor);
        m_stringClass = initializer(stringDescriptor);
        checkStructure();
    }

    String* intern(llvm::StringRef utf8String);

    String* intern(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);
};
} // namespace jllvm

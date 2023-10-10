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

#include <llvm/ExecutionEngine/Orc/Layer.h>

#include <jllvm/object/StringInterner.hpp>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Layer for compiling a JVM method to LLVM IR and handing it to an IR Layer for further compilation.
class ByteCodeCompileLayer : public ByteCodeLayer
{
    StringInterner& m_stringInterner;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;

public:
    ByteCodeCompileLayer(StringInterner& stringInterner, llvm::orc::IRLayer& baseLayer,
                         llvm::orc::MangleAndInterner& mangler, const llvm::DataLayout& dataLayout)
        : ByteCodeLayer(mangler), m_stringInterner(stringInterner), m_baseLayer(baseLayer), m_dataLayout(dataLayout)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
              const ClassFile* classFile, const Method* method, const ClassObject* classObject) override;
};
} // namespace jllvm

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

#include "ByteCodeOSRLayer.hpp"

namespace jllvm
{
/// Layer for compiling OSR versions of methods at a given bytecode to LLVM IR.
class ByteCodeOSRCompileLayer : public ByteCodeOSRLayer
{
    StringInterner& m_stringInterner;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;

public:
    ByteCodeOSRCompileLayer(StringInterner& stringInterner, llvm::orc::IRLayer& baseLayer,
                            llvm::orc::MangleAndInterner& mangler, const llvm::DataLayout& dataLayout)
        : ByteCodeOSRLayer(mangler), m_stringInterner(stringInterner), m_baseLayer(baseLayer), m_dataLayout(dataLayout)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method* method,
              std::uint16_t offset) override;
};
} // namespace jllvm

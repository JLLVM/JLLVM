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

#include "ByteCodeLayer.hpp"

namespace jllvm
{

/// Layer for creating adaptors allowing implementations using the JIT calling convention to be reused with the
/// interpreter calling convention.
class Interpreter2JITLayer : public ByteCodeLayer
{
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;
    llvm::orc::JITDylib& m_i2jAdaptors;

public:
    Interpreter2JITLayer(llvm::orc::IRLayer& baseLayer, llvm::orc::MangleAndInterner& interner,
                         const llvm::DataLayout& dataLayout);

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method* method) override;
};

} // namespace jllvm

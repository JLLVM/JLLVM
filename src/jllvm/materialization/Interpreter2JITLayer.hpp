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
class Interpreter2JITLayer
{
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;
    llvm::orc::JITDylib& m_i2jAdaptors;
    llvm::orc::MangleAndInterner& m_interner;

public:
    Interpreter2JITLayer(llvm::orc::IRLayer& baseLayer, llvm::orc::MangleAndInterner& interner,
                         const llvm::DataLayout& dataLayout);

    /// Registers an implementation of 'method' within 'dylib' conforming to the interpreter calling convention.
    /// Any calls will be translated to the JIT calling convention and calls 'method' within 'jitCCDylib'.
    /// If 'jitCCDylib' does not contain an implementation of 'method' using the JIT calling convention the behaviour
    /// is undefined.
    llvm::Error add(llvm::orc::JITDylib& dylib, const Method& method, llvm::orc::JITDylib& jitCCDylib);

    llvm::orc::MangleAndInterner& getInterner() const
    {
        return m_interner;
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method& method,
              llvm::orc::JITDylib& jitCCDylib);
};

} // namespace jllvm

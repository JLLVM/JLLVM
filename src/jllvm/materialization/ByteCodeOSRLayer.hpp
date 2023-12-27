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

#include <llvm/ExecutionEngine/Orc/Mangling.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/compiler/Compiler.hpp>
#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{
/// Layer for compiling a JVM method at a given bytecode offset for OSR to LLVM IR and handing it to an IR Layer for
/// further compilation.
class ByteCodeOSRLayer
{
    llvm::orc::MangleAndInterner& m_interner;

public:
    explicit ByteCodeOSRLayer(llvm::orc::MangleAndInterner& mangler) : m_interner(mangler) {}

    virtual ~ByteCodeOSRLayer() = default;
    ByteCodeOSRLayer(const ByteCodeOSRLayer&) = delete;
    ByteCodeOSRLayer& operator=(const ByteCodeOSRLayer&) = delete;
    ByteCodeOSRLayer(ByteCodeOSRLayer&&) = delete;
    ByteCodeOSRLayer& operator=(ByteCodeOSRLayer&&) = delete;

    llvm::orc::MangleAndInterner& getInterner() const
    {
        return m_interner;
    }

    /// Method called by the JIT to emit the requested symbols.
    virtual void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method* method,
                      std::uint16_t byteCodeOffset, CallingConvention callingConvention) = 0;

    /// Adds a materialization unit for the given method at the given bytecode offset to 'dylib'.
    llvm::Error add(llvm::orc::JITDylib& dylib, const Method* method, std::uint16_t byteCodeOffset,
                    CallingConvention callingConvention);
};

} // namespace jllvm

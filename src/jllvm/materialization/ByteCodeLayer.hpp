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
#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{
/// Base class layer for any layers operating on JVM Bytecode.
class ByteCodeLayer
{
protected:
    llvm::orc::MangleAndInterner& m_interner;

public:
    explicit ByteCodeLayer(llvm::orc::MangleAndInterner& mangler) : m_interner(mangler) {}

    virtual ~ByteCodeLayer() = default;
    ByteCodeLayer(const ByteCodeLayer&) = delete;
    ByteCodeLayer& operator=(const ByteCodeLayer&) = delete;
    ByteCodeLayer(ByteCodeLayer&&) = delete;
    ByteCodeLayer& operator=(ByteCodeLayer&&) = delete;

    /// Method called by the JIT to emit the requested symbols.
    virtual void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
                      const ClassFile* classFile, const Method* method, const ClassObject* classObject) = 0;

    /// Adds a materialization unit for the given method and class file to 'dylib'.
    llvm::Error add(llvm::orc::JITDylib& dylib, const MethodInfo* methodInfo, const ClassFile* classFile,
                    const Method* method, const ClassObject* classObject);

    /// Returns the map of symbols provided by the method and class file.
    llvm::orc::MaterializationUnit::Interface getSymbolsProvided(const MethodInfo* methodInfo,
                                                                 const ClassFile* classFile);
};

} // namespace jllvm

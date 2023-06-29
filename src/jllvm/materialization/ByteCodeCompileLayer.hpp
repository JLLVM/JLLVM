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

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>

#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/vm/StringInterner.hpp>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Layer for compiling a JVM method to LLVM IR and handing it to an IR Layer for further compilation.
class ByteCodeCompileLayer : public ByteCodeLayer
{
    ClassLoader& m_classLoader;
    StringInterner& m_stringInterner;
    llvm::orc::JITDylib& m_mainDylib;
    std::unique_ptr<llvm::orc::IndirectStubsManager> m_stubsManager;
    llvm::orc::JITCompileCallbackManager& m_callbackManager;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::orc::JITDylib& m_stubsImplDylib;
    llvm::DataLayout m_dataLayout;

public:
    ByteCodeCompileLayer(ClassLoader& classLoader, StringInterner& stringInterner, llvm::orc::JITDylib& mainDylib,
                         std::unique_ptr<llvm::orc::IndirectStubsManager>&& stubsManager,
                         llvm::orc::JITCompileCallbackManager& callbackManager, llvm::orc::IRLayer& baseLayer,
                         llvm::orc::MangleAndInterner& mangler, const llvm::DataLayout& dataLayout)
        : ByteCodeLayer(mangler),
          m_classLoader(classLoader),
          m_stringInterner(stringInterner),
          m_stubsManager(std::move(stubsManager)),
          m_callbackManager(callbackManager),
          m_baseLayer(baseLayer),
          m_stubsImplDylib(baseLayer.getExecutionSession().createBareJITDylib("<stubsImpl>")),
          m_dataLayout(dataLayout),
          m_mainDylib(mainDylib)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
              const ClassFile* classFile, const Method* method, const ClassObject* classObject) override;
};
} // namespace jllvm

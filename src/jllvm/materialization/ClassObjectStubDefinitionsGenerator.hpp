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

#include <jllvm/object/ClassLoader.hpp>

namespace jllvm
{

/// Definitions generator of the JIT used to provide definitions for any call to functions from the
/// 'ClassObjectStubMangling.hpp' mangling functions.
/// Both the class object loading and the definition generation is performed lazily on-demand when a stub is first
/// called.
class ClassObjectStubDefinitionsGenerator : public llvm::orc::DefinitionGenerator
{
    std::unique_ptr<llvm::orc::IndirectStubsManager> m_stubsManager;
    llvm::orc::JITCompileCallbackManager& m_callbackManager;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::orc::JITDylib& m_impl;
    llvm::DataLayout m_dataLayout;
    ClassLoader& m_classLoader;
    ClassObject* m_objectClassCache = nullptr;

public:
    explicit ClassObjectStubDefinitionsGenerator(std::unique_ptr<llvm::orc::IndirectStubsManager> stubsManager,
                                                 llvm::orc::JITCompileCallbackManager& callbackManager,
                                                 llvm::orc::IRLayer& baseLayer, const llvm::DataLayout& dataLayout,
                                                 llvm::orc::JITDylib& attachedTo, ClassLoader& classLoader)
        : m_stubsManager(std::move(stubsManager)),
          m_callbackManager(callbackManager),
          m_baseLayer(baseLayer),
          m_impl(m_baseLayer.getExecutionSession().createBareJITDylib("<classObjectStubs>")),
          m_dataLayout(dataLayout),
          m_classLoader(classLoader)
    {
        attachedTo.withLinkOrderDo([&](const llvm::orc::JITDylibSearchOrder& dylibSearchOrder)
                                   { m_impl.setLinkOrder(dylibSearchOrder); });
    }

    llvm::Error tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& dylib,
                              llvm::orc::JITDylibLookupFlags,
                              const llvm::orc::SymbolLookupSet& symbolLookupSet) override;
};

} // namespace jllvm

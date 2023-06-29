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

#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/LazyReexports.h>

#include <unordered_map>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
class ByteCodeOnDemandLayer : public ByteCodeLayer
{
public:
    /// Builder for IndirectStubsManagers.
    using IndirectStubsManagerBuilder = std::function<std::unique_ptr<llvm::orc::IndirectStubsManager>()>;

private:
    ByteCodeLayer& m_baseLayer;
    llvm::orc::ExecutionSession& m_session;
    mutable std::mutex m_mutex;
    IndirectStubsManagerBuilder m_builder;
    llvm::orc::LazyCallThroughManager& m_callThroughManager;

    // For every target JITDylib we need to create a mirroring JITDylib that contains the actual method bodies.
    struct PerDylibResources
    {
        PerDylibResources(llvm::orc::JITDylib& impl, std::unique_ptr<llvm::orc::IndirectStubsManager> stubs)
            : m_impl(impl), m_stubs(std::move(stubs))
        {
        }

        llvm::orc::JITDylib& getImplDylib()
        {
            return m_impl;
        }

        llvm::orc::IndirectStubsManager& getStubs()
        {
            return *m_stubs;
        }

    private:
        llvm::orc::JITDylib& m_impl;
        std::unique_ptr<llvm::orc::IndirectStubsManager> m_stubs;
    };

    // Not DenseMap as we need the address stability on insert.
    std::unordered_map<const llvm::orc::JITDylib*, PerDylibResources> m_resources;

    PerDylibResources& getPerDylibResources(llvm::orc::JITDylib& target);

public:
    ByteCodeOnDemandLayer(ByteCodeLayer& baseLayer, llvm::orc::ExecutionSession& session,
                          llvm::orc::MangleAndInterner& mangler, IndirectStubsManagerBuilder builder,
                          llvm::orc::LazyCallThroughManager& callThroughManager)
        : ByteCodeLayer(mangler),
          m_session(session),
          m_baseLayer(baseLayer),
          m_builder(std::move(builder)),
          m_callThroughManager(callThroughManager)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
              const ClassFile* classFile, const Method* method, const ClassObject* classObject) override;
};

} // namespace jllvm

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

#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>

#include <jllvm/gc/GarbageCollector.hpp>

namespace jllvm
{
/// JIT link plugin for extracting the LLVM generated stack map section out of materialized objects and notifying
/// the GC about newly added entries.
class StackMapRegistrationPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
{
    jllvm::GarbageCollector& m_gc;

public:
    explicit StackMapRegistrationPlugin(jllvm::GarbageCollector& gc) : m_gc(gc) {}

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override;

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override;

    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey, llvm::orc::ResourceKey) override {}

    void modifyPassConfig(llvm::orc::MaterializationResponsibility&, llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override;
};
} // namespace jllvm

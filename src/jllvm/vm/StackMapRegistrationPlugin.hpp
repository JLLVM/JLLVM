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

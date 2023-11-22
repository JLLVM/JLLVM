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
#include <llvm/Object/StackMapParser.h>

#include <jllvm/gc/GarbageCollector.hpp>

#include <utility>

#include "JIT.hpp"

namespace jllvm
{
/// JIT link plugin for extracting the LLVM generated stack map section out of materialized objects and notifying
/// the GC about newly added entries.
class StackMapRegistrationPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
{
    GarbageCollector& m_gc;
    llvm::DenseSet<std::uintptr_t>& m_javaFrameSet;
    llvm::StringRef m_stackMapSection;
    llvm::StringRef m_javaSection;
    llvm::DenseMap<llvm::orc::ResourceKey, std::vector<JavaMethodMetadata::JITData*>> m_needsCleanup;

    using StackMapParser = llvm::StackMapParser<llvm::support::endianness::native>;

    /// Converts a location in the stackmap into an equivalent 'FrameValue<T>'.
    template <class T>
    FrameValue<T> toFrameValue(const StackMapParser::LocationAccessor& loc, StackMapParser& parser);

    /// Converts a location in the stackmap into an equivalent 'WriteableFrameValue<T>'.
    template <class T>
    std::optional<WriteableFrameValue<T>> toWriteableFrameValue(const StackMapParser::LocationAccessor& loc);

    void parseJITEntry(JavaMethodMetadata::JITData& jitData, StackMapParser::RecordAccessor& record,
                       StackMapParser& parser, std::uint64_t functionAddress);

public:
    explicit StackMapRegistrationPlugin(GarbageCollector& gc, llvm::DenseSet<std::uintptr_t>& javaFrameSet)
        : m_gc(gc), m_javaFrameSet(javaFrameSet)
    {
        m_javaSection = "java";
        if (llvm::Triple(LLVM_HOST_TRIPLE).isOSBinFormatMachO())
        {
            m_javaSection = "__TEXT,java";
        }

        m_stackMapSection = ".llvm_stackmaps";
        if (llvm::Triple(LLVM_HOST_TRIPLE).isOSBinFormatMachO())
        {
            m_stackMapSection = "__LLVM_STACKMAPS,__llvm_stackmaps";
        }
    }

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override;

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override;

    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey, llvm::orc::ResourceKey) override {}

    void modifyPassConfig(llvm::orc::MaterializationResponsibility&, llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override;
};
} // namespace jllvm

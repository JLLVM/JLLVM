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

#include "StackMapRegistrationPlugin.hpp"

#include <llvm/ADT/ScopeExit.h>

llvm::Error jllvm::StackMapRegistrationPlugin::notifyFailed(llvm::orc::MaterializationResponsibility&)
{
    return llvm::Error::success();
}

llvm::Error jllvm::StackMapRegistrationPlugin::notifyRemovingResources(llvm::orc::JITDylib&,
                                                                       llvm::orc::ResourceKey resourceKey)
{
    auto iter = m_needsCleanup.find(resourceKey);
    if (iter != m_needsCleanup.end())
    {
        // Call the destructors of any JIT metadata that was placed in the removed code sections.
        for (const JavaMethodMetadata::JITData* jitData : iter->second)
        {
            jitData->JavaMethodMetadata::JITData::~JITData();
        }
        m_needsCleanup.erase(iter);
    }
    return llvm::Error::success();
}

template <class T>
jllvm::FrameValue<T> jllvm::StackMapRegistrationPlugin::toFrameValue(const StackMapParser::LocationAccessor& loc,
                                                                     StackMapParser& parser)
{
    switch (loc.getKind())
    {
        case StackMapParser::LocationKind::Register: return FrameValue<T>::inRegister(loc.getDwarfRegNum());
        case StackMapParser::LocationKind::Direct: return FrameValue<T>::direct(loc.getDwarfRegNum(), loc.getOffset());
        case StackMapParser::LocationKind::Indirect:
            return FrameValue<T>::indirect(loc.getSizeInBytes(), loc.getDwarfRegNum(), loc.getOffset());
        case StackMapParser::LocationKind::Constant: return FrameValue<T>::constant(loc.getSmallConstant());
        case StackMapParser::LocationKind::ConstantIndex:
            return FrameValue<T>::constant(parser.getConstant(loc.getConstantIndex()).getValue());
    }
    llvm_unreachable("invalid kind");
}

template <class T>
std::optional<jllvm::WriteableFrameValue<T>>
    jllvm::StackMapRegistrationPlugin::toWriteableFrameValue(const StackMapParser::LocationAccessor& loc)
{
    switch (loc.getKind())
    {
        case StackMapParser::LocationKind::Register: return WriteableFrameValue<T>::inRegister(loc.getDwarfRegNum());
        case StackMapParser::LocationKind::Indirect:
            return WriteableFrameValue<T>::indirect(loc.getSizeInBytes(), loc.getDwarfRegNum(), loc.getOffset());
        case StackMapParser::LocationKind::Constant:
        case StackMapParser::LocationKind::ConstantIndex: return std::nullopt;
        default: llvm_unreachable("location is not writeable");
    }
}

void jllvm::StackMapRegistrationPlugin::parseJITEntry(JavaMethodMetadata::JITData& jitData,
                                                      StackMapParser::RecordAccessor& record, StackMapParser& parser,
                                                      std::uint64_t functionAddress)
{
    enum
    {
        DeoptCountPos = 2,
        BytecodeDeoptPos = 3,
        NumLocalsPos = 4,
        LocalsStartPos = 5,
    };

    assert(record.getLocation(DeoptCountPos).getSmallConstant() != 0 && "jit frame must have deopt values");

    std::uint64_t addr = functionAddress + record.getInstructionOffset();
    std::uint16_t byteCodeOffset = record.getLocation(BytecodeDeoptPos).getSmallConstant();
    std::uint16_t numLocals = record.getLocation(NumLocalsPos).getSmallConstant();

    std::vector<FrameValue<std::uint64_t>> locals(numLocals);
    for (std::uint32_t i = LocalsStartPos; i < LocalsStartPos + numLocals; i++)
    {
        locals[i - LocalsStartPos] = toFrameValue<std::uint64_t>(record.getLocation(i), parser);
    }

    jitData.insert(addr, {byteCodeOffset, std::move(locals)});
}

void jllvm::StackMapRegistrationPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility& mr,
                                                         llvm::jitlink::LinkGraph&,
                                                         llvm::jitlink::PassConfiguration& config)
{
    llvm::orc::ResourceKey resourceKey;
    llvm::cantFail(mr.withResourceKeyDo([&](llvm::orc::ResourceKey key) { resourceKey = key; }));

    // Prevent the stackmap symbol from being garbage collected by marking it as alive prior to pruning.
    config.PrePrunePasses.emplace_back(
        [&](llvm::jitlink::LinkGraph& g)
        {
            llvm::jitlink::Section* section = g.findSectionByName(m_stackMapSection);
            if (!section)
            {
                return llvm::Error::success();
            }
            auto res = llvm::find_if(section->symbols(), [&](const llvm::jitlink::Symbol* symbol)
                                     { return symbol->hasName() && symbol->getName() == "__LLVM_StackMaps"; });
            if (res != section->symbols().end())
            {
                (*res)->setLive(true);
            }
            return llvm::Error::success();
        });

    config.PostAllocationPasses.emplace_back(
        [&](llvm::jitlink::LinkGraph& g)
        {
            llvm::jitlink::Section* section = g.findSectionByName(m_javaSection);
            if (!section)
            {
                return llvm::Error::success();
            }

            // Collect the addresses of all Java functions.
            for (llvm::jitlink::Symbol* iter : section->symbols())
            {
                m_javaFrameSet.insert(iter->getAddress().getValue());
            }

            return llvm::Error::success();
        });

    // After post fix up all relocations have been replaced with the absolute addresses, which is the perfect time
    // to extract the stackmap and get all the instruction pointers.
    config.PostFixupPasses.emplace_back(
        [&, resourceKey](llvm::jitlink::LinkGraph& g)
        {
            llvm::jitlink::Section* section = g.findSectionByName(m_stackMapSection);
            if (!section)
            {
                return llvm::Error::success();
            }
            auto range = llvm::jitlink::SectionRange(*section);

            auto* start = range.getStart().toPtr<std::uint8_t*>();
            std::size_t size = range.getSize();

            StackMapParser parser({start, size});

            llvm::SmallVector<StackMapEntry> entries;
            JavaMethodMetadata::JITData* jitData = nullptr;
            std::size_t recordCount = 0;
            auto currFunc = parser.functions_begin();
            std::uint64_t functionAddress = currFunc->getFunctionAddress();
            bool isJavaFrame = m_javaFrameSet.contains(functionAddress);
            for (auto&& record : parser.records())
            {
                auto atExit = llvm::make_scope_exit(
                    [&]
                    {
                        if (++recordCount == currFunc->getRecordCount())
                        {
                            currFunc++;
                            jitData = nullptr;
                            recordCount = 0;
                            functionAddress = currFunc->getFunctionAddress();
                            isJavaFrame = m_javaFrameSet.contains(functionAddress);
                        }
                    });

                // Java frames need to additionally have their metadata prefix data populated. After linking is done,
                // the memory is made read-only making any initialization afterward impossible.
                if (isJavaFrame)
                {
                    JavaMethodMetadata& metadata = reinterpret_cast<JavaMethodMetadata*>(functionAddress)[-1];
                    switch (metadata.getKind())
                    {
                        case JavaMethodMetadata::Kind::JIT:
                            if (!jitData)
                            {
                                jitData = &metadata.emplaceJITData();
                                m_needsCleanup[resourceKey].push_back(jitData);
                            }
                            parseJITEntry(*jitData, record, parser, functionAddress);
                            break;
                        case JavaMethodMetadata::Kind::Native: break;
                    }
                }

                entries.clear();
                std::uint32_t deoptCount = record.getLocation(2).getSmallConstant();
                for (std::uint16_t i = 3 + deoptCount; i < record.getNumLocations(); i += 2)
                {
                    std::optional<WriteableFrameValue<ObjectInterface*>> basePtr =
                        toWriteableFrameValue<ObjectInterface*>(record.getLocation(i));
                    std::optional<WriteableFrameValue<std::byte*>> derivedPtr =
                        toWriteableFrameValue<std::byte*>(record.getLocation(i + 1));
                    if (!derivedPtr)
                    {
                        continue;
                    }
                    assert(basePtr
                           && "writeable derived pointer without writeable base pointer should not be possible");
                    entries.push_back({*basePtr, *derivedPtr});
                }
                std::uint64_t addr = functionAddress + record.getInstructionOffset();
                m_gc.addStackMapEntries(addr, entries);
            }

            return llvm::Error::success();
        });
}

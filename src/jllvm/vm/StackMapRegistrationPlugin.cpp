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

#include <llvm/Object/StackMapParser.h>

llvm::Error jllvm::StackMapRegistrationPlugin::notifyFailed(llvm::orc::MaterializationResponsibility&)
{
    return llvm::Error::success();
}

llvm::Error jllvm::StackMapRegistrationPlugin::notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey)
{
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

void jllvm::StackMapRegistrationPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility&,
                                                         llvm::jitlink::LinkGraph&,
                                                         llvm::jitlink::PassConfiguration& config)
{
    // Prevent the stackmap symbol from being garbage collected by marking it as alive prior to pruning.
    config.PrePrunePasses.emplace_back(
        [&](llvm::jitlink::LinkGraph& g)
        {
            llvm::StringRef stackMapSectionName = ".llvm_stackmaps";
            if (g.getTargetTriple().isOSBinFormatMachO())
            {
                stackMapSectionName = "__LLVM_STACKMAPS,__llvm_stackmaps";
            }
            llvm::jitlink::Section* section = g.findSectionByName(stackMapSectionName);
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

    // After post fix up all relocations have been replaced with the absolute addresses, which is the perfect time
    // to extract the stackmap and get all the instruction pointers.
    config.PostFixupPasses.emplace_back(
        [&](llvm::jitlink::LinkGraph& g)
        {
            llvm::StringRef stackMapSectionName = ".llvm_stackmaps";
            if (g.getTargetTriple().isOSBinFormatMachO())
            {
                stackMapSectionName = "__LLVM_STACKMAPS,__llvm_stackmaps";
            }
            llvm::jitlink::Section* section = g.findSectionByName(stackMapSectionName);
            if (!section)
            {
                return llvm::Error::success();
            }
            auto range = llvm::jitlink::SectionRange(*section);

            auto* start = range.getStart().toPtr<std::uint8_t*>();
            std::size_t size = range.getSize();

            llvm::StackMapParser<llvm::support::endianness::native> parser({start, size});

            llvm::SmallVector<StackMapEntry> entries;
            auto currFunc = parser.functions_begin();
            std::size_t recordCount = 0;
            for (auto&& record : parser.records())
            {
                std::uintptr_t addr = currFunc->getFunctionAddress() + record.getInstructionOffset();

                if (++recordCount == currFunc->getRecordCount())
                {
                    currFunc++;
                    recordCount = 0;
                }

                entries.clear();
                std::uint32_t deoptCount = record.getLocation(2).getSmallConstant();
                if (deoptCount != 0)
                {
                    std::uint16_t bytecodeOffset = record.getLocation(3).getSmallConstant();
                    std::uint16_t numLocals = record.getLocation(4).getSmallConstant();
                    JIT::DeoptEntry entry{bytecodeOffset};
                    entry.locals.reserve(numLocals);
                    for (std::uint32_t i = 5; i < 5 + numLocals; i++)
                    {
                        auto loc = record.getLocation(i);
                        entry.locals.push_back(toFrameValue<std::uint64_t>(loc, parser));
                    }

                    m_deoptEntryParsed(addr, std::move(entry));
                }

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
                m_gc.addStackMapEntries(addr, entries);
            }

            return llvm::Error::success();
        });
}

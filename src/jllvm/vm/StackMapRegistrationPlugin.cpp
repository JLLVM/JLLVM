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

            llvm::SmallVector<jllvm::StackMapEntry> entries;
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
                for (std::uint16_t i = 3; i < record.getNumLocations(); i++)
                {
                    auto loc = record.getLocation(i);
                    switch (loc.getKind())
                    {
                        case decltype(parser)::LocationKind::Register:
                        case decltype(parser)::LocationKind::Direct:
                        case decltype(parser)::LocationKind::Indirect:
                        {
                            entries.push_back({static_cast<jllvm::StackMapEntry::Type>(loc.getKind()),
                                               static_cast<std::uint8_t>(loc.getSizeInBytes() / sizeof(void*)),
                                               loc.getDwarfRegNum(),
                                               loc.getKind() != decltype(parser)::LocationKind::Register ?
                                                   static_cast<std::uint32_t>(loc.getOffset()) :
                                                   0});
                            break;
                        }
                        case decltype(parser)::LocationKind::Constant:
                        case decltype(parser)::LocationKind::ConstantIndex: continue;
                    }
                }
                llvm::sort(entries);
                entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
                m_gc.addStackMapEntries(addr, entries);
            }

            return llvm::Error::success();
        });
}

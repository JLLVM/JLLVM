#include "ByteCodeOnDemandLayer.hpp"

#include <llvm/ExecutionEngine/Orc/LazyReexports.h>

jllvm::ByteCodeOnDemandLayer::PerDylibResources&
    jllvm::ByteCodeOnDemandLayer::getPerDylibResources(llvm::orc::JITDylib& target)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    if (auto result = m_resources.find(&target); result != m_resources.end())
    {
        return result->second;
    }

    llvm::orc::JITDylib& impl = m_session.createBareJITDylib(target.getName() + ".impl");
    llvm::orc::JITDylibSearchOrder newLinkOrder;
    target.withLinkOrderDo([&](const llvm::orc::JITDylibSearchOrder& targetLinkOrder)
                           { newLinkOrder = targetLinkOrder; });

    newLinkOrder.insert(std::next(newLinkOrder.begin()), {&impl, llvm::orc::JITDylibLookupFlags::MatchAllSymbols});
    impl.setLinkOrder(newLinkOrder, false);
    target.setLinkOrder(std::move(newLinkOrder), false);

    return m_resources.insert({&target, PerDylibResources{impl, m_builder()}}).first->second;
}

void jllvm::ByteCodeOnDemandLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                        const jllvm::MethodInfo* methodInfo, const jllvm::ClassFile* classFile)
{
    auto& pdr = getPerDylibResources(mr->getTargetJITDylib());

    llvm::orc::SymbolAliasMap symbols;
    for (const auto& [name, flags] : mr->getSymbols())
    {
        symbols[name] = llvm::orc::SymbolAliasMapEntry(name, flags);
    }

    // Add materialization unit to the implementation dylib.
    llvm::cantFail(m_baseLayer.add(pdr.getImplDylib(), methodInfo, classFile));

    // Use a lazy-reexport to create the required symbols instead. The reexport will emit the stubs in this dylib
    // satisfying the dynamic linker. Once they are called, lookups are done in the implementation dylib causing
    // materialization and therefore compilation through our layers.
    llvm::cantFail(mr->replace(
        llvm::orc::lazyReexports(m_callThroughManager, pdr.getStubs(), pdr.getImplDylib(), std::move(symbols))));
}

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
                                        const Method* method)
{
    auto& pdr = getPerDylibResources(mr->getTargetJITDylib());

    llvm::orc::SymbolAliasMap symbols;
    for (const auto& [name, flags] : mr->getSymbols())
    {
        symbols[name] = llvm::orc::SymbolAliasMapEntry(name, flags);
    }

    // Add materialization unit to the implementation dylib.
    llvm::cantFail(m_baseLayer.add(pdr.getImplDylib(), method));

    // Use a lazy-reexport to create the required symbols instead. The reexport will emit the stubs in this dylib
    // satisfying the dynamic linker. Once they are called, lookups are done in the implementation dylib causing
    // materialization and therefore compilation through our layers.
    llvm::cantFail(mr->replace(
        llvm::orc::lazyReexports(m_callThroughManager, pdr.getStubs(), pdr.getImplDylib(), std::move(symbols))));
}

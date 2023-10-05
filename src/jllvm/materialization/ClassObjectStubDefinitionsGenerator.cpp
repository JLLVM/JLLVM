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

#include "ClassObjectStubDefinitionsGenerator.hpp"

#include "ClassObjectStubCodeGenerator.hpp"

namespace
{
using namespace jllvm;

/// Compiles the given 'variant' to its corresponding function definition and returns the new module containing the
/// function definition.
llvm::orc::ThreadSafeModule compile(const DemangledVariant& variant, ClassLoader& classLoader,
                                    const llvm::DataLayout& dataLayout)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("name", *context);

    module->setDataLayout(dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    match(
        variant,
        [&](const DemangledFieldAccess& fieldAccess)
        {
            ClassObject& classObject = classLoader.forName(ObjectType(fieldAccess.className));
            generateFieldAccessStub(*module, classObject, fieldAccess.fieldName, fieldAccess.descriptor);
        },
        [](auto) { llvm_unreachable("Not yet implemented"); });
    return llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
}

} // namespace

llvm::Error jllvm::ClassObjectStubDefinitionsGenerator::tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind,
                                                                      llvm::orc::JITDylib& dylib,
                                                                      llvm::orc::JITDylibLookupFlags,
                                                                      const llvm::orc::SymbolLookupSet& symbolLookupSet)
{
    // Map of symbols generated by this invocation.
    llvm::orc::SymbolMap generated;
    for (const llvm::orc::SymbolStringPtr& symbol : llvm::make_first_range(symbolLookupSet))
    {
        // The name has C mangling already applied to it in the form of a global prefix. Remove it if present.
        llvm::StringRef name = *symbol;
        if (name.front() == m_dataLayout.getGlobalPrefix())
        {
            name = name.drop_front();
        }

        // Attempt to demangle the name. If it is a monostate then the symbol is not a stub and there is nothing to do.
        DemangledVariant demangleVariant = demangleStubSymbolName(name);
        if (holds_alternative<std::monostate>(demangleVariant))
        {
            continue;
        }

        // Otherwise, create a stub containing a compiler callback. The compile callback will be called on the very
        // first invocation of the symbol and will redirect the stub to point to the compiled function. This
        // effectively implements both the lazy compilation and lazy class loading.
        llvm::cantFail(m_stubsManager->createStub(
            name,
            llvm::cantFail(m_callbackManager.getCompileCallback(
                [this, demangleVariant, name, symbol]
                {
                    llvm::orc::ThreadSafeModule module = compile(demangleVariant, m_classLoader, m_dataLayout);

                    llvm::cantFail(m_baseLayer.add(m_impl, std::move(module)));
                    llvm::JITTargetAddress address =
                        llvm::cantFail(m_baseLayer.getExecutionSession().lookup({&m_impl}, symbol)).getAddress();
                    llvm::cantFail(m_stubsManager->updatePointer(name, address));
                    return address;
                })),
            llvm::JITSymbolFlags::Exported));
        generated[symbol] = m_stubsManager->findStub(name, true);
    }

    if (generated.empty())
    {
        return llvm::Error::success();
    }

    return dylib.define(llvm::orc::absoluteSymbols(std::move(generated)));
}

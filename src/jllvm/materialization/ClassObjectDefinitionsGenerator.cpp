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

#include "ClassObjectDefinitionsGenerator.hpp"

#include <jllvm/compiler/ClassObjectStubMangling.hpp>

llvm::Error jllvm::ClassObjectDefinitionsGenerator::tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind,
                                                                  llvm::orc::JITDylib& dylib,
                                                                  llvm::orc::JITDylibLookupFlags,
                                                                  const llvm::orc::SymbolLookupSet& symbolLookupSet)
{
    llvm::orc::SymbolMap generated;
    for (const llvm::orc::SymbolStringPtr& symbol : llvm::make_first_range(symbolLookupSet))
    {
        // The name has C mangling already applied to it in the form of a global prefix. Remove it if present.
        llvm::StringRef name = *symbol;
        if (name.front() == m_dataLayout.getGlobalPrefix())
        {
            name = name.drop_front();
        }

        DemangledVariant demangleVariant = demangleStubSymbolName(name);
        auto* classObjectGlobal = get_if<DemangledClassObjectGlobal>(&demangleVariant);
        if (!classObjectGlobal)
        {
            continue;
        }

        generated[symbol] =
            llvm::JITEvaluatedSymbol::fromPointer(&m_classLoader.forName(classObjectGlobal->classObject));
    }
    if (generated.empty())
    {
        return llvm::Error::success();
    }

    return dylib.define(llvm::orc::absoluteSymbols(std::move(generated)));
}

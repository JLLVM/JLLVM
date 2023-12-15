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

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/IR/DataLayout.h>

#include <jllvm/object/ClassLoader.hpp>

namespace jllvm
{

/// Definitions generator used to resolve any global references to class objects in LLVM IR.
class ClassObjectDefinitionsGenerator : public llvm::orc::DefinitionGenerator
{
    ClassLoader& m_classLoader;
    llvm::DataLayout m_dataLayout;

public:
    ClassObjectDefinitionsGenerator(ClassLoader& classLoader, const llvm::DataLayout& dataLayout)
        : m_classLoader(classLoader), m_dataLayout(dataLayout)
    {
    }

    llvm::Error tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& dylib,
                              llvm::orc::JITDylibLookupFlags,
                              const llvm::orc::SymbolLookupSet& symbolLookupSet) override;
};
} // namespace jllvm

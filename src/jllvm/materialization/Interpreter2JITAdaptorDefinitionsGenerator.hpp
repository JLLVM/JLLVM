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
#include <llvm/ExecutionEngine/Orc/Layer.h>

namespace jllvm
{

/// Definitions generator lazily generating adaptor functions for converting from interpreter to JIT calling convention.
/// Names of functions have to conform to the following grammar:
/// <name> ::= '(' { <type> } ')' <ret-type>
/// <type> ::= <basic-type-descriptor> | 'L'
/// <ret-type> ::= <type> | 'V'
///
/// The name is designed to be derivable from a method type descriptor with the exception that 1) the 'this' parameter
/// is part of the type and has to be explicitly added by adding an 'L' character and 2) all reference parameters
/// including arrays are reduced to just 'L'.
class Interpreter2JITAdaptorDefinitionsGenerator : public llvm::orc::DefinitionGenerator
{
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;

public:
    Interpreter2JITAdaptorDefinitionsGenerator(llvm::orc::IRLayer& baseLayer, const llvm::DataLayout& dataLayout)
        : m_baseLayer(baseLayer), m_dataLayout(dataLayout)
    {
    }

    llvm::Error tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& dylib,
                              llvm::orc::JITDylibLookupFlags,
                              const llvm::orc::SymbolLookupSet& symbolLookupSet) override;
};

} // namespace jllvm

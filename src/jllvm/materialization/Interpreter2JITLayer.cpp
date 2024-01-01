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

#include "Interpreter2JITLayer.hpp"

#include <llvm/IR/IRBuilder.h>

#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>

#include "Interpreter2JITAdaptorDefinitionsGenerator.hpp"

jllvm::Interpreter2JITLayer::Interpreter2JITLayer(llvm::orc::IRLayer& baseLayer, llvm::orc::MangleAndInterner& interner,
                                                  const llvm::DataLayout& dataLayout)
    : ByteCodeLayer(interner),
      m_baseLayer{baseLayer},
      m_dataLayout{dataLayout},
      m_i2jAdaptors{baseLayer.getExecutionSession().createBareJITDylib("<i2jAdaptors>")}
{
    m_i2jAdaptors.addGenerator(std::make_unique<Interpreter2JITAdaptorDefinitionsGenerator>(baseLayer, dataLayout));
}

void jllvm::Interpreter2JITLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const Method* method)
{
    // Perform mangling from the method type to the adaptor names.
    MethodType methodType = method->getType();
    std::string mangling = "(";
    if (!method->isStatic())
    {
        mangling += "L";
    }
    auto addToMangling = [&](FieldType fieldType)
    {
        if (fieldType.isReference())
        {
            mangling += "L";
            return;
        }
        mangling += fieldType.textual();
    };
    llvm::for_each(methodType.parameters(), addToMangling);
    mangling += ")";
    addToMangling(methodType.returnType());

    auto [methodName, flags] = *mr->getSymbols().begin();
    llvm::cantFail(mr->replace(llvm::orc::reexports(
        m_i2jAdaptors, {{methodName, llvm::orc::SymbolAliasMapEntry(getInterner()(mangling), flags)}})));
}

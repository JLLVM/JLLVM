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
    : m_interner{interner},
      m_baseLayer{baseLayer},
      m_dataLayout{dataLayout},
      m_i2jAdaptors{baseLayer.getExecutionSession().createBareJITDylib("<i2jAdaptors>")}
{
    m_i2jAdaptors.addGenerator(std::make_unique<Interpreter2JITAdaptorDefinitionsGenerator>(baseLayer, dataLayout));
}

namespace
{
using namespace jllvm;

class Interpreter2JITMaterializationUnit : public llvm::orc::MaterializationUnit
{
    Interpreter2JITLayer& m_layer;
    const Method& m_method;
    llvm::orc::JITDylib& m_jitCCDylib;

public:
    Interpreter2JITMaterializationUnit(Interpreter2JITLayer& layer, const Method& method,
                                       llvm::orc::JITDylib& jitCcDylib)
        : llvm::orc::MaterializationUnit(
              [&]
              {
                  llvm::orc::SymbolFlagsMap result;
                  auto name = mangleDirectMethodCall(&method);
                  result[layer.getInterner()(name)] = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
                  return llvm::orc::MaterializationUnit::Interface(std::move(result), nullptr);
              }()),
          m_layer(layer),
          m_method(method),
          m_jitCCDylib(jitCcDylib)
    {
    }

    llvm::StringRef getName() const override
    {
        return "Interpreter2JITMaterializationUnit";
    }

    void materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr) override
    {
        m_layer.emit(std::move(mr), m_method, m_jitCCDylib);
    }

private:
    void discard(const llvm::orc::JITDylib&, const llvm::orc::SymbolStringPtr&) override
    {
        llvm_unreachable("Should not be possible");
    }
};

} // namespace

llvm::Error jllvm::Interpreter2JITLayer::add(llvm::orc::JITDylib& dylib, const Method& method,
                                             llvm::orc::JITDylib& jitCCDylib)
{
    return dylib.define(std::make_unique<Interpreter2JITMaterializationUnit>(*this, method, jitCCDylib));
}

void jllvm::Interpreter2JITLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const Method& method, llvm::orc::JITDylib& jitCCDylib)
{
    // Perform mangling from the method type to the adaptor names.
    MethodType methodType = method.getType();
    std::string mangling = "(";
    if (!method.isStatic())
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

    // Fetch both the adaptor and the callee in the JIT calling convention.
    llvm::orc::ExecutionSession& session = mr->getExecutionSession();
    llvm::JITTargetAddress adaptor =
        llvm::cantFail(session.lookup({&m_i2jAdaptors}, m_interner(mangling))).getAddress();
    std::string symbol = mangleDirectMethodCall(&method);
    llvm::JITTargetAddress jitCCSymbol = llvm::cantFail(session.lookup({&jitCCDylib}, m_interner(symbol))).getAddress();

    // Implement the interpreter calling convention symbol by creating a lambda that just forwards the arguments and
    // JIT CC symbol to the adaptor.
    llvm::cantFail(mr->replace(createLambdaMaterializationUnit(
        symbol, m_baseLayer,
        [=](const Method*, const std::uint64_t* arguments) -> std::uint64_t
        {
            return reinterpret_cast<std::uint64_t (*)(void*, const std::uint64_t*)>(adaptor)(
                reinterpret_cast<void*>(jitCCSymbol), arguments);
        },
        m_dataLayout, m_interner)));
}

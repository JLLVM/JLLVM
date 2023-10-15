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

#include "ClassObjectStubCodeGenerator.hpp"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>

#include "ByteCodeCompileUtils.hpp"

namespace
{

class TrivialDebugInfoBuilder
{
    llvm::DIBuilder m_debugBuilder;
    llvm::DISubprogram* m_subProgram;

public:
    TrivialDebugInfoBuilder(llvm::Function* function) : m_debugBuilder(*function->getParent())
    {
        llvm::DIFile* file = m_debugBuilder.createFile(".", ".");
        m_debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

        m_subProgram =
            m_debugBuilder.createFunction(file, function->getName(), "", file, 1,
                                          m_debugBuilder.createSubroutineType(m_debugBuilder.getOrCreateTypeArray({})),
                                          1, llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);

        function->setSubprogram(m_subProgram);
    }

    ~TrivialDebugInfoBuilder()
    {
        finalize();
    }

    TrivialDebugInfoBuilder(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder(TrivialDebugInfoBuilder&&) = delete;
    TrivialDebugInfoBuilder& operator=(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder& operator=(TrivialDebugInfoBuilder&&) = delete;

    void finalize()
    {
        if (!m_subProgram)
        {
            return;
        }
        m_debugBuilder.finalizeSubprogram(std::exchange(m_subProgram, nullptr));
        m_debugBuilder.finalize();
    }
};

void buildClassInitializerInitStub(llvm::IRBuilder<>& builder, const jllvm::ClassObject& classObject)
{
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::Module* module = function->getParent();

    llvm::Value* classObjectLLVM =
        builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(&classObject)), builder.getPtrTy());
    auto* initializedGEP = builder.CreateGEP(builder.getInt8Ty(), classObjectLLVM,
                                             builder.getInt32(jllvm::ClassObject::getInitializedOffset()));
    auto* initialized =
        builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), initializedGEP), builder.getInt8(0));

    auto* classInitializer = llvm::BasicBlock::Create(builder.getContext(), "", function);
    auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "", function);
    builder.CreateCondBr(initialized, continueBlock, classInitializer);

    builder.SetInsertPoint(classInitializer);

    builder.CreateCall(
        module->getOrInsertFunction("jllvm_initialize_class_object", builder.getVoidTy(), classObjectLLVM->getType()),
        classObjectLLVM);

    builder.CreateBr(continueBlock);

    builder.SetInsertPoint(continueBlock);
}

} // namespace

llvm::Function* jllvm::generateFieldAccessStub(llvm::Module& module, const ClassObject& classObject,
                                               llvm::StringRef fieldName, jllvm::FieldType descriptor)
{
    const Field* field = classObject.getField(fieldName, descriptor);
    assert(field && "Expected class object to contain the given field");

    llvm::Type* returnType;
    if (field->isStatic())
    {
        // Note that this is a normal pointer, not a reference as whatever value is loaded from it
        // is the potential Object Reference.
        returnType = llvm::PointerType::get(module.getContext(), 0);
    }
    else
    {
        returnType = llvm::IntegerType::get(module.getContext(), sizeof(std::size_t) * 8);
    }
    auto* functionType = llvm::FunctionType::get(returnType, /*isVarArg=*/false);

    auto* function =
        llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                               mangleFieldAccess(classObject.getClassName(), fieldName, descriptor), module);
    applyABIAttributes(function);

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));

    // Static field accesses trigger class object initializations.
    if (field->isStatic() && !classObject.isInitialized())
    {
        buildClassInitializerInitStub(builder, classObject);
    }

    llvm::Value* returnValue;
    if (field->isStatic())
    {
        returnValue = builder.CreateIntToPtr(
            builder.getInt64(reinterpret_cast<std::uint64_t>(field->getAddressOfStatic())), returnType);
    }
    else
    {
        returnValue = llvm::ConstantInt::get(returnType, field->getOffset());
    }

    builder.CreateRet(returnValue);

    return function;
}

llvm::Function* jllvm::generateMethodResolutionCallStub(llvm::Module& /*module*/,
                                                        jllvm::MethodResolution /*resolution*/,
                                                        const ClassObject& /*classObject*/,
                                                        llvm::StringRef /*fieldName*/, jllvm::MethodType /*descriptor*/)
{
    llvm_unreachable("not yet implemented");
}

llvm::Function* jllvm::generateStaticCallStub(llvm::Module& /*module*/, const ClassObject& /*classObject*/,
                                              llvm::StringRef /*methodName*/, jllvm::MethodType /*descriptor*/)
{
    llvm_unreachable("not yet implemented");
}

llvm::Function* jllvm::generateClassObjectAccessStub(llvm::Module& module, const ClassObject& classObject)
{
    auto* functionType = llvm::FunctionType::get(referenceType(module.getContext()), /*isVarArg=*/false);

    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                            mangleClassObjectAccess(classObject.getDescriptor()), module);
    applyABIAttributes(function);

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));

    llvm::Value* pointer = builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uint64_t>(&classObject)),
                                                  function->getReturnType());
    builder.CreateRet(pointer);

    return function;
}

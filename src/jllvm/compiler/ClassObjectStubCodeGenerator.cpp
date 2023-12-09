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

#include <llvm/IR/IRBuilder.h>

#include <jllvm/debuginfo/TrivialDebugInfoBuilder.hpp>

#include "ByteCodeCompileUtils.hpp"

namespace
{

void buildClassInitializerInitStub(llvm::IRBuilder<>& builder, const jllvm::ClassObject& classObject)
{
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::Module* module = function->getParent();

    llvm::Value* classObjectLLVM = jllvm::classObjectGlobal(*module, classObject.getDescriptor());
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
        classObjectLLVM, llvm::OperandBundleDef("deopt", std::nullopt));

    builder.CreateBr(continueBlock);

    builder.SetInsertPoint(continueBlock);
}

/// Builds LLVM IR returning the result of the call with the assumption that the call's return type matches the
/// containing functions.
void buildRetCall(llvm::IRBuilder<>& builder, llvm::CallInst* call)
{
    if (call->getType()->isVoidTy())
    {
        builder.CreateRetVoid();
    }
    else
    {
        builder.CreateRet(call);
    }
}

/// Builds LLVM IR to create a direct call to 'method' with the given arguments.
llvm::CallInst* buildDirectMethodCall(llvm::IRBuilder<>& builder, const jllvm::Method* method,
                                      llvm::ArrayRef<llvm::Value*> args)
{
    llvm::Module& module = *builder.GetInsertBlock()->getParent()->getParent();
    llvm::FunctionCallee callee = module.getOrInsertFunction(
        mangleDirectMethodCall(method),
        jllvm::descriptorToType(method->getType(), method->isStatic(), builder.getContext()));
    applyABIAttributes(llvm::cast<llvm::Function>(callee.getCallee()), method->getType(), method->isStatic());
    llvm::CallInst* call = builder.CreateCall(callee, args, llvm::OperandBundleDef("deopt", std::nullopt));
    applyABIAttributes(call, method->getType(), method->isStatic());
    return call;
}

/// Returns a new global constant named 'mangledName' initialized from static final contents of 'field' given by its
/// type (only primitive JVM types are allowed)
llvm::GlobalVariable* createGlobalConstant(llvm::Module& module, llvm::StringRef mangledName, const jllvm::Field* field)
{
    auto* initializer = jllvm::match(
        field->getType(),
        [&](jllvm::BaseType baseType) -> llvm::Constant*
        {
            switch (baseType.getValue())
            {
                case jllvm::BaseType::Void: break;
                case jllvm::BaseType::Boolean:
                case jllvm::BaseType::Byte:
                    return llvm::ConstantInt::get(llvm::Type::getInt8Ty(module.getContext()),
                                                  *static_cast<const std::int8_t*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Short:
                    return llvm::ConstantInt::get(llvm::Type::getInt16Ty(module.getContext()),
                                                  *static_cast<const std::int16_t*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Char:
                    return llvm::ConstantInt::get(llvm::Type::getInt16Ty(module.getContext()),
                                                  *static_cast<const std::uint16_t*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Double:
                    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(module.getContext()),
                                                 *static_cast<const double*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Float:
                    return llvm::ConstantFP::get(llvm::Type::getFloatTy(module.getContext()),
                                                 *static_cast<const float*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Int:
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(module.getContext()),
                                                  *static_cast<const std::int32_t*>(field->getAddressOfStatic()));
                case jllvm::BaseType::Long:
                    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),
                                                  *static_cast<const std::int64_t*>(field->getAddressOfStatic()));
            }
            llvm_unreachable("Fields of void type are not allowed");
        },
        [&](auto) -> llvm::Constant* { llvm_unreachable("Reference types are not allowed to be cached"); });

    return new llvm::GlobalVariable(module, initializer->getType(), /*isConstant=*/true,
                                    llvm::GlobalValue::InternalLinkage, initializer, mangledName);
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
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    // Static field accesses trigger class object initializations.
    if (field->isStatic() && classObject.isUnintialized())
    {
        buildClassInitializerInitStub(builder, classObject);
    }

    llvm::Value* returnValue;
    if (field->isStatic())
    {
        // Only sound if field is static, final, initialized and not a reference type
        if (field->isFinal() && classObject.isInitialized() && !descriptor.isReference())
        {
            returnValue = createGlobalConstant(
                module, mangleFieldAccess(classObject.getClassName(), fieldName, descriptor), field);
        }
        else
        {
            returnValue = builder.CreateIntToPtr(
                builder.getInt64(reinterpret_cast<std::uint64_t>(field->getAddressOfStatic())), returnType);
        }
    }
    else
    {
        returnValue = llvm::ConstantInt::get(returnType, field->getOffset());
    }

    builder.CreateRet(returnValue);

    return function;
}

llvm::Function* jllvm::generateMethodResolutionCallStub(llvm::Module& module, jllvm::MethodResolution resolution,
                                                        const ClassObject& classObject, llvm::StringRef methodName,
                                                        jllvm::MethodType descriptor, const ClassObject& objectClass)
{
    auto* functionType = descriptorToType(descriptor, /*isStatic=*/false, module.getContext());

    auto* function = llvm::Function::Create(
        functionType, llvm::GlobalValue::ExternalLinkage,
        mangleMethodResolutionCall(resolution, classObject.getClassName(), methodName, descriptor), module);
    applyABIAttributes(function, descriptor, /*isStatic=*/false);

    llvm::SmallVector<llvm::Value*> args = llvm::to_vector_of<llvm::Value*>(llvm::make_pointer_range(function->args()));

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    const Method* resolvedMethod;
    switch (resolution)
    {
        case MethodResolution::Virtual: resolvedMethod = classObject.methodResolution(methodName, descriptor); break;
        case MethodResolution::Interface:
            resolvedMethod = classObject.interfaceMethodResolution(methodName, descriptor, &objectClass);
            break;
    }

    if (!resolvedMethod->getTableSlot())
    {
        buildRetCall(builder, buildDirectMethodCall(builder, resolvedMethod, args));
        return function;
    }

    if (!resolvedMethod->getClassObject()->isInterface())
    {
        llvm::Value* methodOffset = builder.getInt32(sizeof(VTableSlot) * *resolvedMethod->getTableSlot());
        llvm::Value* thisClassObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
        llvm::Value* vtblPositionInClassObject = builder.getInt32(ClassObject::getVTableOffset());

        llvm::Value* totalOffset = builder.CreateAdd(vtblPositionInClassObject, methodOffset);
        llvm::Value* vtblSlot = builder.CreateGEP(builder.getInt8Ty(), thisClassObject, {totalOffset});
        llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), vtblSlot);

        auto* call = builder.CreateCall(functionType, callee, args, llvm::OperandBundleDef("deopt", std::nullopt));
        applyABIAttributes(call, descriptor, /*isStatic=*/false);
        buildRetCall(builder, call);
        return function;
    }

    std::size_t sizeTBits = std::numeric_limits<std::size_t>::digits;
    llvm::Value* slot = builder.getIntN(sizeTBits, *resolvedMethod->getTableSlot());
    llvm::Value* id = builder.getIntN(sizeTBits, resolvedMethod->getClassObject()->getInterfaceId());

    llvm::Value* thisClassObject = builder.CreateLoad(referenceType(builder.getContext()), args.front());
    llvm::Value* iTablesPtr =
        builder.CreateGEP(builder.getInt8Ty(), thisClassObject, {builder.getInt32(ClassObject::getITablesOffset())});
    llvm::Value* iTables =
        builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(arrayRefType(builder.getContext()), iTablesPtr,
                                                                 {builder.getInt32(0), builder.getInt32(0)}));

    // Linear search over all iTables of 'classObject' until the iTable with the interface id equal to
    // 'id' is found.
    llvm::BasicBlock* pred = builder.GetInsertBlock();
    auto* loopBody = llvm::BasicBlock::Create(builder.getContext(), "", pred->getParent());
    builder.CreateBr(loopBody);

    builder.SetInsertPoint(loopBody);
    llvm::PHINode* phi = builder.CreatePHI(builder.getInt32Ty(), 2);
    phi->addIncoming(builder.getInt32(0), pred);

    llvm::Value* iTable = builder.CreateLoad(builder.getPtrTy(), builder.CreateGEP(builder.getPtrTy(), iTables, {phi}));
    llvm::Value* iTableId = builder.CreateLoad(slot->getType(), iTable);
    llvm::Value* cond = builder.CreateICmpEQ(iTableId, id);
    llvm::Value* increment = builder.CreateAdd(phi, builder.getInt32(1));
    phi->addIncoming(increment, loopBody);

    auto* loopContinue = llvm::BasicBlock::Create(builder.getContext(), "", pred->getParent());
    builder.CreateCondBr(cond, loopContinue, loopBody);

    builder.SetInsertPoint(loopContinue);

    llvm::Value* iTableSlot =
        builder.CreateGEP(iTableType(builder.getContext()), iTable, {builder.getInt32(0), builder.getInt32(1), slot});
    llvm::Value* callee = builder.CreateLoad(builder.getPtrTy(), iTableSlot);

    auto* call = builder.CreateCall(functionType, callee, args, llvm::OperandBundleDef("deopt", std::nullopt));
    applyABIAttributes(call, descriptor, /*isStatic=*/false);
    buildRetCall(builder, call);

    return function;
}

llvm::Function* jllvm::generateSpecialMethodCallStub(llvm::Module& module, const ClassObject& classObject,
                                                     llvm::StringRef methodName, MethodType descriptor,
                                                     const jllvm::ClassObject* callerClass,
                                                     const jllvm::ClassObject& objectClass)
{
    auto* functionType = descriptorToType(descriptor, /*isStatic=*/false, module.getContext());

    auto* function = llvm::Function::Create(
        functionType, llvm::GlobalValue::ExternalLinkage,
        mangleSpecialMethodCall(classObject.getClassName(), methodName, descriptor,
                                callerClass ? callerClass->getDescriptor() : std::optional<FieldType>{}),
        module);
    applyABIAttributes(function, descriptor, /*isStatic=*/false);

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    const Method* method = classObject.specialMethodResolution(methodName, descriptor, &objectClass, callerClass);

    // 'invokespecial' does not do method selection like the others.
    // The spec mentions it as explicitly invoking the resolved method.
    buildRetCall(builder,
                 buildDirectMethodCall(builder, method,
                                       llvm::to_vector_of<llvm::Value*>(llvm::make_pointer_range(function->args()))));

    return function;
}

llvm::Function* jllvm::generateStaticCallStub(llvm::Module& module, const ClassObject& classObject,
                                              llvm::StringRef methodName, MethodType descriptor,
                                              const ClassObject& objectClass)
{
    auto* functionType = descriptorToType(descriptor, /*isStatic=*/true, module.getContext());

    auto* function =
        llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                               mangleStaticCall(classObject.getClassName(), methodName, descriptor), module);
    applyABIAttributes(function, descriptor, /*isStatic=*/true);

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    if (classObject.isUnintialized())
    {
        buildClassInitializerInitStub(builder, classObject);
    }

    const Method* method = classObject.isInterface() ?
                               classObject.interfaceMethodResolution(methodName, descriptor, &objectClass) :
                               classObject.methodResolution(methodName, descriptor);

    buildRetCall(builder,
                 buildDirectMethodCall(builder, method,
                                       llvm::to_vector_of<llvm::Value*>(llvm::make_pointer_range(function->args()))));

    return function;
}

llvm::Function* jllvm::generateClassObjectAccessStub(llvm::Module& module, FieldType classObject)
{
    auto* functionType = llvm::FunctionType::get(referenceType(module.getContext()), /*isVarArg=*/false);

    auto* function = llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                            mangleClassObjectAccess(classObject), module);
    applyABIAttributes(function);

    TrivialDebugInfoBuilder debugInfoBuilder(function);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    llvm::Value* pointer = classObjectGlobal(module, classObject);
    builder.CreateRet(pointer);

    return function;
}

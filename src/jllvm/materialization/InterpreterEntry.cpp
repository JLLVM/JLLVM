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

#include "InterpreterEntry.hpp"

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>

llvm::Value* jllvm::generateInterpreterEntry(
    llvm::IRBuilder<>& builder, const Method& method,
    llvm::function_ref<void(llvm::IRBuilder<>&, llvm::AllocaInst*, llvm::AllocaInst*, llvm::AllocaInst*,
                            llvm::AllocaInst*, llvm::AllocaInst*, llvm::AllocaInst*, const Code& code)>
        generatePrologue)
{
    llvm::Module& module = *builder.GetInsertBlock()->getParent()->getParent();
    Code* code = method.getMethodInfo().getAttributes().find<Code>();
    assert(code && "cannot run method without code");

    // Allocate all the variables for the interpretation context.
    llvm::AllocaInst* byteCodeOffset = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* topOfStack = builder.CreateAlloca(builder.getInt16Ty());
    llvm::AllocaInst* operandStack =
        builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), code->getMaxStack()));
    llvm::AllocaInst* operandGCMask =
        builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), llvm::divideCeil(code->getMaxStack(), 64)));
    llvm::AllocaInst* localVariables =
        builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), code->getMaxLocals()));
    llvm::AllocaInst* localVariablesGCMask =
        builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), llvm::divideCeil(code->getMaxLocals(), 64)));
    llvm::Value* methodRef = methodGlobal(module, &method);

    generatePrologue(builder, byteCodeOffset, topOfStack, operandStack, operandGCMask, localVariables,
                     localVariablesGCMask, *code);

    std::array<llvm::Value*, 7> arguments = {methodRef,     byteCodeOffset, topOfStack,          operandStack,
                                             operandGCMask, localVariables, localVariablesGCMask};
    std::array<llvm::Type*, 7> types{};
    llvm::transform(arguments, types.begin(), std::mem_fn(&llvm::Value::getType));

    // Deopt all allocas used as context during interpretation. This makes it possible for the unwinder to read the
    // local variables, the operand stack, the bytecode offset and where GC pointers are contained during unwinding.
    llvm::CallInst* callInst = builder.CreateCall(
        module.getOrInsertFunction("jllvm_interpreter",
                                   llvm::FunctionType::get(builder.getInt64Ty(), types, /*isVarArg=*/false)),
        arguments, llvm::OperandBundleDef("deopt", llvm::ArrayRef(arguments).drop_front()));

    llvm::Type* returnType = descriptorToType(method.getType().returnType(), builder.getContext());
    if (returnType->isVoidTy())
    {
        return nullptr;
    }

    // Translate the uint64_t returned by the interpreter to the corresponding type in the C calling convention.
    llvm::TypeSize typeSize = module.getDataLayout().getTypeSizeInBits(returnType);
    assert(!typeSize.isScalable() && "return type is never a scalable type");

    llvm::Value* value = callInst;
    llvm::IntegerType* intNTy = builder.getIntNTy(typeSize.getFixedValue());
    if (intNTy != value->getType())
    {
        value = builder.CreateTrunc(value, intNTy);
    }
    return builder.CreateBitOrPointerCast(value, returnType);
}

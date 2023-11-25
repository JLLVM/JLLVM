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

#include "JIT2InterpreterLayer.hpp"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>

void jllvm::JIT2InterpreterLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const Method* method)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("module", *context);
    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    llvm::DIBuilder debugBuilder(*module);
    llvm::DIFile* file = debugBuilder.createFile(".", ".");

    debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

    auto* function =
        llvm::Function::Create(descriptorToType(method->getType(), method->isStatic(), *context),
                               llvm::GlobalValue::ExternalLinkage, mangleDirectMethodCall(method), module.get());

    llvm::DISubprogram* subprogram =
        debugBuilder.createFunction(file, function->getName(), function->getName(), file, 1,
                                    debugBuilder.createSubroutineType(debugBuilder.getOrCreateTypeArray({})), 1,
                                    llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    function->setSubprogram(subprogram);

    applyABIAttributes(function, method->getType(), method->isStatic());
    function->clearGC();
    addJavaMethodMetadata(function, method, JavaMethodMetadata::Kind::Interpreter);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

    builder.SetCurrentDebugLocation(llvm::DILocation::get(builder.getContext(), 1, 1, subprogram));

    Code* code = method->getMethodInfo().getAttributes().find<Code>();
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
    llvm::Value* methodRef = methodGlobal(*module, method);

    // Zero init all the allocas.
    builder.CreateMemSet(byteCodeOffset, builder.getInt8(0), sizeof(std::uint16_t), std::nullopt);
    builder.CreateMemSet(topOfStack, builder.getInt8(0), sizeof(std::uint16_t), std::nullopt);
    builder.CreateMemSet(operandStack, builder.getInt8(0), sizeof(std::uint64_t) * code->getMaxStack(), std::nullopt);
    builder.CreateMemSet(operandGCMask, builder.getInt8(0),
                         sizeof(std::uint64_t) * llvm::divideCeil(code->getMaxStack(), 64), std::nullopt);
    builder.CreateMemSet(localVariables, builder.getInt8(0), sizeof(std::uint64_t) * code->getMaxLocals(),
                         std::nullopt);
    builder.CreateMemSet(localVariablesGCMask, builder.getInt8(0), llvm::divideCeil(code->getMaxLocals(), 64),
                         std::nullopt);

    // Initialize the local variables from the function arguments. This does the translation from the C calling
    // convention to the interpreters local variables.
    // If the argument being stored into the local variable is a reference type, the corresponding bit is set in the GC
    // mask as well.
    std::size_t localVariableIndex = 0;
    if (!method->isStatic())
    {
        // Store the 'this' argument.
        llvm::Value* gep = builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariables, localVariableIndex);
        builder.CreateStore(function->getArg(0), gep);
        llvm::Value* maskGep =
            builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariablesGCMask, localVariableIndex / 64);
        llvm::Value* value = builder.CreateLoad(builder.getInt64Ty(), maskGep);
        value = builder.CreateOr(value, builder.getInt64(1 << (localVariableIndex % 64)));
        builder.CreateStore(value, maskGep);
        localVariableIndex++;
    }

    for (auto&& [argument, paramType] :
         llvm::zip_equal(llvm::drop_begin(function->args(), localVariableIndex), method->getType().parameters()))
    {
        llvm::Value* gep = builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariables, localVariableIndex);
        builder.CreateStore(&argument, gep);
        if (paramType.isReference())
        {
            llvm::Value* maskGep =
                builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariablesGCMask, localVariableIndex / 64);
            llvm::Value* value = builder.CreateLoad(builder.getInt64Ty(), maskGep);
            value = builder.CreateOr(value, builder.getInt64(1 << (localVariableIndex % 64)));
            builder.CreateStore(value, maskGep);
        }

        localVariableIndex++;
        if (paramType.isWide())
        {
            localVariableIndex++;
        }
    }

    std::array<llvm::Value*, 7> arguments = {methodRef,     byteCodeOffset, topOfStack,          operandStack,
                                             operandGCMask, localVariables, localVariablesGCMask};
    std::array<llvm::Type*, 7> types{};
    llvm::transform(arguments, types.begin(), std::mem_fn(&llvm::Value::getType));

    // Deopt all allocas used as context during interpretation. This makes it possible for the unwinder to read the
    // local variables, the operand stack, the bytecode offset and where GC pointers are contained during unwinding.
    llvm::CallInst* callInst = builder.CreateCall(
        module->getOrInsertFunction("jllvm_interpreter",
                                    llvm::FunctionType::get(builder.getInt64Ty(), types, /*isVarArg=*/false)),
        arguments, llvm::OperandBundleDef("deopt", llvm::ArrayRef(arguments).drop_front()));

    if (function->getReturnType()->isVoidTy())
    {
        builder.CreateRetVoid();
    }
    else
    {
        // Translate the uint64_t returned by the interpreter to the corresponding type in the C calling convention.
        llvm::TypeSize typeSize = m_dataLayout.getTypeSizeInBits(function->getReturnType());
        assert(!typeSize.isScalable() && "return type is never a scalable type");

        llvm::Value* value = callInst;
        llvm::IntegerType* intNTy = builder.getIntNTy(typeSize.getFixedValue());
        if (intNTy != value->getType())
        {
            value = builder.CreateTrunc(value, intNTy);
        }
        value = builder.CreateBitOrPointerCast(value, function->getReturnType());
        builder.CreateRet(value);
    }

    debugBuilder.finalizeSubprogram(subprogram);
    debugBuilder.finalize();

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

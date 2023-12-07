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

#include "InterpreterOSRLayer.hpp"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>

#include "InterpreterEntry.hpp"

void jllvm::InterpreterOSRLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                      const Method* method, std::uint16_t offset)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("module", *context);
    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    llvm::DIBuilder debugBuilder(*module);
    llvm::DIFile* file = debugBuilder.createFile(".", ".");

    debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

    auto* function =
        llvm::Function::Create(osrMethodSignature(method->getType(), *context), llvm::GlobalValue::ExternalLinkage,
                               mangleOSRMethod(method, offset), module.get());

    llvm::DISubprogram* subprogram =
        debugBuilder.createFunction(file, function->getName(), function->getName(), file, 1,
                                    debugBuilder.createSubroutineType(debugBuilder.getOrCreateTypeArray({})), 1,
                                    llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    function->setSubprogram(subprogram);

    applyABIAttributes(function);
    function->clearGC();
    addJavaMethodMetadata(function, method, JavaMethodMetadata::Kind::Interpreter);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

    builder.SetCurrentDebugLocation(llvm::DILocation::get(builder.getContext(), 1, 1, subprogram));

    llvm::Value* returnValue = generateInterpreterEntry(
        builder, *method,
        [&](llvm::IRBuilder<>& builder, llvm::AllocaInst* byteCodeOffset, llvm::AllocaInst* topOfStack,
            llvm::AllocaInst* operandStack, llvm::AllocaInst* operandGCMask, llvm::AllocaInst* localVariables,
            llvm::AllocaInst* localVariablesGCMask, const Code& code)
        {
            llvm::Value* osrState = function->getArg(0);

            // Initialize the abstract machine state from the OSR State. The precise layout of 'osrState' is documented
            // in 'OSRState::release'. The code below simply traverses through the array, always adding offsets to the
            // current pointer value and copying the values over to the interpreter frame.

            builder.CreateStore(builder.CreateLoad(builder.getInt16Ty(), osrState), byteCodeOffset);
            llvm::Value* operandStackSize = builder.CreateLShr(builder.CreateLoad(builder.getInt32Ty(), osrState), 16);
            builder.CreateStore(builder.CreateTrunc(operandStackSize, builder.getInt16Ty()), topOfStack);

            llvm::Value* localVariablesSrc = builder.CreateConstGEP1_32(builder.getInt64Ty(), osrState, 1);
            builder.CreateMemCpy(localVariables, /*DstAlign=*/std::nullopt, localVariablesSrc,
                                 /*SrcAlign=*/std::nullopt, code.getMaxLocals() * sizeof(std::uint64_t));

            llvm::Value* operandStackSrc =
                builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariablesSrc, code.getMaxLocals());
            builder.CreateMemCpy(operandStack, /*DstAlign=*/std::nullopt, operandStackSrc, /*SrcAlign=*/std::nullopt,
                                 builder.CreateMul(operandStackSize, builder.getInt32(sizeof(std::uint64_t))));

            llvm::Value* localVariablesGCMaskSrc =
                builder.CreateGEP(builder.getInt64Ty(), operandStackSrc, operandStackSize);
            std::uint64_t localVariablesGCMaskSize = llvm::divideCeil(code.getMaxLocals(), 64);
            builder.CreateMemCpy(localVariablesGCMask, /*DstAlign=*/std::nullopt, localVariablesGCMaskSrc,
                                 /*SrcAlign=*/std::nullopt, localVariablesGCMaskSize * sizeof(std::uint64_t));

            // Calculate the operand stack GC mask size from the operand stack size. This is a 'ceil(size / 64)'
            // operation implemented in IR as "size / 64 + ((size % 64) != 0)".
            llvm::Value* operandGCMaskSize = operandStackSize;
            operandGCMaskSize = builder.CreateUDiv(operandGCMaskSize, builder.getInt32(64));
            llvm::Value* remainder = builder.CreateURem(operandStackSize, builder.getInt32(64));
            operandGCMaskSize = builder.CreateAdd(
                operandGCMaskSize,
                builder.CreateZExt(builder.CreateICmpNE(remainder, builder.getInt32(0)), builder.getInt32Ty()));
            operandGCMaskSize = builder.CreateMul(operandGCMaskSize, builder.getInt32(sizeof(std::uint64_t)));

            llvm::Value* operandGCMaskSrc =
                builder.CreateConstGEP1_32(builder.getInt64Ty(), localVariablesGCMaskSrc, localVariablesGCMaskSize);
            builder.CreateMemCpy(operandGCMask, /*DstAlign=*/std::nullopt, operandGCMaskSrc, /*SrcAlign=*/std::nullopt,
                                 operandGCMaskSize);

            // The OSR frame is responsible for deleting its input arrays as the frame that originally allocated the
            // pointer is replaced.
            llvm::FunctionCallee callee = function->getParent()->getOrInsertFunction(
                "jllvm_osr_frame_delete", builder.getVoidTy(), builder.getPtrTy());
            llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr("gc-leaf-function");
            builder.CreateCall(callee, osrState);
        });
    if (returnValue)
    {
        builder.CreateRet(returnValue);
    }
    else
    {
        builder.CreateRetVoid();
    }

    debugBuilder.finalizeSubprogram(subprogram);
    debugBuilder.finalize();

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

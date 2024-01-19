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
#include <jllvm/debuginfo/TrivialDebugInfoBuilder.hpp>

void jllvm::JIT2InterpreterLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const Method* method)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("module", *context);
    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    auto* function =
        llvm::Function::Create(descriptorToType(method->getType(), method->isStatic(), *context),
                               llvm::GlobalValue::ExternalLinkage, mangleDirectMethodCall(method), module.get());

    TrivialDebugInfoBuilder debugInfoBuilder(function);

    applyABIAttributes(function, method->getType(), method->isStatic());
    function->clearGC();

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    std::size_t argumentArrayCount = 0;
    if (!method->isStatic())
    {
        argumentArrayCount++;
    }
    for (FieldType parameter : method->getType().parameters())
    {
        argumentArrayCount++;
        if (parameter.isWide())
        {
            argumentArrayCount++;
        }
    }

    llvm::Value* argumentArray = builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), argumentArrayCount));
    // Zero out argument array for any unassigned bytes.
    builder.CreateMemSet(argumentArray, builder.getInt8(0), argumentArrayCount * sizeof(std::uint64_t),
                         llvm::MaybeAlign());
    std::size_t index = 0;
    for (llvm::Argument& arg : function->args())
    {
        llvm::Value* gep = builder.CreateConstGEP1_32(builder.getInt64Ty(), argumentArray, index);
        builder.CreateStore(&arg, gep);
        index++;
        if (arg.getType()->isDoubleTy() || arg.getType()->isIntegerTy(64))
        {
            index++;
        }
    }

    llvm::Value* value = builder.CreateCall(module->getOrInsertFunction("jllvm_interpreter_entry", builder.getInt64Ty(),
                                                                        builder.getPtrTy(0), builder.getPtrTy(0)),
                                            {methodGlobal(*module, method), argumentArray});

    if (function->getReturnType()->isVoidTy())
    {
        builder.CreateRetVoid();
    }
    else
    {
        // Translate the uint64_t returned by the interpreter to the corresponding type in the JIT calling convention.
        llvm::TypeSize typeSize = module->getDataLayout().getTypeSizeInBits(function->getReturnType());
        assert(!typeSize.isScalable() && "return type is never a scalable type");

        llvm::IntegerType* intNTy = builder.getIntNTy(typeSize.getFixedValue());
        if (intNTy != value->getType())
        {
            value = builder.CreateTrunc(value, intNTy);
        }
        builder.CreateRet(builder.CreateBitOrPointerCast(value, function->getReturnType()));
    }

    debugInfoBuilder.finalize();

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

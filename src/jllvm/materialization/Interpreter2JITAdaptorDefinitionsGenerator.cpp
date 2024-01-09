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

#include "Interpreter2JITAdaptorDefinitionsGenerator.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/debuginfo/TrivialDebugInfoBuilder.hpp>

namespace
{

struct Signature
{
    std::vector<llvm::Type*> parameters;
    llvm::Type* returnType;
};

/// Parses a signature as seen in the description of 'Interpreter2JITAdaptorDefinitionsGenerator' into its LLVM types.
/// Returns an empty optional if it does not match the grammar.
std::optional<Signature> parseSignature(llvm::StringRef name, llvm::LLVMContext& context)
{
    if (name.front() != '(')
    {
        return std::nullopt;
    }

    name = name.drop_front();

    auto convertType = [&](char c) -> llvm::Type*
    {
        switch (c)
        {
            case 'V': return llvm::Type::getVoidTy(context);
            case 'B': return llvm::Type::getInt8Ty(context);
            case 'C': return llvm::Type::getInt16Ty(context);
            case 'D': return llvm::Type::getDoubleTy(context);
            case 'F': return llvm::Type::getFloatTy(context);
            case 'I': return llvm::Type::getInt32Ty(context);
            case 'J': return llvm::Type::getInt64Ty(context);
            case 'S': return llvm::Type::getInt16Ty(context);
            case 'Z': return llvm::Type::getInt8Ty(context);
            case 'L': return jllvm::referenceType(context);
            default: return nullptr;
        }
    };

    std::vector<llvm::Type*> parameters;
    for (; !name.empty() && name.front() != ')'; name = name.drop_front())
    {
        // Void parameters are not allowed.
        if (name.front() == 'V')
        {
            return std::nullopt;
        }

        llvm::Type* paramType = convertType(name.front());
        if (!paramType)
        {
            return std::nullopt;
        }
        parameters.push_back(paramType);
    }
    if (name.empty() || name.front() != ')')
    {
        return std::nullopt;
    }
    name = name.drop_front();
    if (name.size() != 1)
    {
        return std::nullopt;
    }

    llvm::Type* returnType = convertType(name.front());
    if (!returnType)
    {
        return std::nullopt;
    }
    return Signature{std::move(parameters), returnType};
}

/// Compile an adaptor of the given 'name'. Returns an empty optional if the name does not conform to the grammar.
std::optional<llvm::orc::ThreadSafeModule> compileAdaptor(llvm::StringRef name, const llvm::DataLayout& dataLayout)
{
    auto context = std::make_unique<llvm::LLVMContext>();

    std::optional<Signature> signature = parseSignature(name, *context);
    if (!signature)
    {
        return std::nullopt;
    }

    auto module = std::make_unique<llvm::Module>("name", *context);

    module->setDataLayout(dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::IntegerType::getInt64Ty(*context),
                                {llvm::PointerType::get(*context, 0), llvm::PointerType::get(*context, 0)},
                                /*isVarArg=*/false),
        llvm::GlobalValue::ExternalLinkage, name, *module);
    jllvm::TrivialDebugInfoBuilder debugInfoBuilder(function);

    llvm::Value* functionPointer = function->getArg(0);
    llvm::Value* argumentArray = function->getArg(1);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    llvm::SmallVector<llvm::Value*> arguments;
    std::size_t i = 0;
    for (llvm::Type* loadType : signature->parameters)
    {
        llvm::Value* gep = builder.CreateConstGEP1_32(argumentArray->getType(), argumentArray, i++);
        // 'double' and 'long' take two slots in the arguments array.
        if (loadType->isDoubleTy() || loadType->isIntegerTy(64))
        {
            i++;
        }
        arguments.push_back(builder.CreateLoad(loadType, gep));
    }

    auto* functionType = llvm::FunctionType::get(
        signature->returnType, llvm::to_vector(llvm::map_range(arguments, std::mem_fn(&llvm::Value::getType))),
        /*isVarArg=*/false);

    llvm::Value* call = builder.CreateCall(functionType, functionPointer, arguments);
    if (signature->returnType->isVoidTy())
    {
        // For void methods returning any kind of value would suffice as it is never read.
        // C++ callers do not expect a 'poison' or 'undef' value however (as clang uses 'noundef' and 'nopoison'
        // return attributes), so avoid using those.
        builder.CreateRet(builder.getInt64(0));
    }
    else
    {
        // Translate the value returned by the C calling convention to the 'uint64_t' expected by the interpreter.
        llvm::TypeSize typeSize = dataLayout.getTypeSizeInBits(signature->returnType);
        assert(!typeSize.isScalable() && "return type is never a scalable type");

        llvm::IntegerType* intNTy = builder.getIntNTy(typeSize.getFixedValue());
        call = builder.CreateBitOrPointerCast(call, intNTy);
        call = builder.CreateZExtOrTrunc(call, function->getReturnType());
        builder.CreateRet(call);
    }
    debugInfoBuilder.finalize();

    return llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
}
} // namespace

llvm::Error jllvm::Interpreter2JITAdaptorDefinitionsGenerator::tryToGenerate(
    llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& dylib, llvm::orc::JITDylibLookupFlags,
    const llvm::orc::SymbolLookupSet& symbolLookupSet)
{
    for (const llvm::orc::SymbolStringPtr& symbol : llvm::make_first_range(symbolLookupSet))
    {
        llvm::StringRef name = *symbol;
        if (name.front() == m_dataLayout.getGlobalPrefix())
        {
            name = name.drop_front();
        }

        std::optional<llvm::orc::ThreadSafeModule> module = compileAdaptor(name, m_dataLayout);
        if (!module)
        {
            continue;
        }

        llvm::Error error = m_baseLayer.add(dylib, std::move(*module));
        if (error)
        {
            return error;
        }
    }

    return llvm::Error::success();
}

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

#include "JNIImplementationLayer.hpp"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <jllvm/class/Descriptors.hpp>
#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/debuginfo/TrivialDebugInfoBuilder.hpp>

namespace
{
std::string escape(llvm::StringRef string)
{
    std::string result;
    result.reserve(string.size());
    // TODO: Support for UTF-16 code units escaping.
    for (char iter : string)
    {
        switch (iter)
        {
            case '/': result += '_'; break;
            case '_': result += "_1"; break;
            case ';': result += "_2"; break;
            case '[': result += "_3"; break;
            default: result += iter; break;
        }
    }
    return result;
}

} // namespace

std::string jllvm::formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName)
{
    return "Java_" + escape(className) + "_" + escape(methodName);
}

std::string jllvm::formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName, MethodType methodType)
{
    std::string result = formJNIMethodName(className, methodName);
    result += "__";
    // Append just the parameters from the method descriptor.
    for (FieldType parameter : methodType.parameters())
    {
        result += parameter.textual();
    }
    return result;
}

std::string jllvm::formJNIMethodName(const Method* method, bool withType)
{
    if (withType)
    {
        return formJNIMethodName(method->getClassObject()->getClassName(), method->getName(), method->getType());
    }
    return formJNIMethodName(method->getClassObject()->getClassName(), method->getName());
}

void jllvm::JNIImplementationLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                         const Method* method)
{
    // Things that should happen here:
    // 1. Materialize a stub calling a compile callback to be called when the native method is called.
    // 2. Within the callback we need to materialize a trampoline function using LLVM code that does pre-setup for
    //    things like synchronization and possibly in the future GC rooting, creates a JNIENV* to prepend to the
    //    arguments and after the real implementation call, does post-setup for things like synchronization
    //    and exceptions. Within the trampoline materialization we should also look up the real implementation
    //    in 'm_jniImpls' trying the short signature first and the overloaded one second.
    // 3. We replace the stub with the materialized trampoline.

    llvm::orc::SymbolFlagsMap map = mr->getSymbols();

    std::string bridgeName = mangleDirectMethodCall(method);
    // Reference:
    // https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#resolving-native-method-names
    std::string jniName = formJNIMethodName(method, /*withType=*/false);
    auto lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, getInterner()(jniName));
    if (!lookup)
    {
        llvm::consumeError(lookup.takeError());
        jniName = formJNIMethodName(method, /*withType=*/true);
        lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, getInterner()(jniName));
    }

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(bridgeName, *context);
    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);


    MethodType methodType = method->getType();
    auto* function = llvm::Function::Create(descriptorToType(methodType, method->isStatic(), *context),
                                            llvm::GlobalValue::ExternalLinkage, bridgeName, module.get());

    TrivialDebugInfoBuilder debugInfoBuilder(function);

    applyABIAttributes(function, methodType, method->isStatic());
    function->clearGC();
    addJavaNativeMethodMetadata(function, method);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));
    builder.SetCurrentDebugLocation(debugInfoBuilder.getNoopLoc());

    llvm::Type* referenceType = jllvm::referenceType(*context);
    if (lookup)
    {
        // For exception handling, we reuse the exception handler from our C++ implementation. We currently only
        // support implementations using the Itanium ABI with DWARF exception handling. Once we support any other
        // implementations (e.g. Windows), we'll want to write our own personality function.
        llvm::FunctionCallee personalityFn = module->getOrInsertFunction(
            "__gxx_personality_v0", llvm::FunctionType::get(builder.getInt32Ty(), /*isVarArg=*/true));
        function->setPersonalityFn(llvm::cast<llvm::Constant>(personalityFn.getCallee()));

        llvm::Value* environment = builder.CreateAlloca(llvm::StructType::get(builder.getPtrTy()));
        builder.CreateStore(
            builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uintptr_t>(m_jniNativeFunctions)),
                                   builder.getPtrTy()),
            environment);
        builder.CreateCall(module->getOrInsertFunction("jllvm_push_local_frame", builder.getVoidTy()));

        llvm::SmallVector<llvm::Value*> args{environment};
        if (method->isStatic())
        {
            args.push_back(classObjectGlobal(*module, method->getClassObject()->getDescriptor()));
        }

        for (llvm::Argument& arg : function->args())
        {
            args.push_back(&arg);
        }

        for (llvm::Value*& arg : args)
        {
            if (arg->getType() != referenceType)
            {
                continue;
            }
            arg = builder.CreateCall(
                module->getOrInsertFunction("jllvm_new_local_root", arg->getType(), arg->getType()), arg);
        }

        llvm::SmallVector<llvm::Type*> argTypes;
        // Env
        argTypes.push_back(environment->getType());
        // jclass or object
        argTypes.push_back(referenceType);

        constexpr std::size_t parameterStartOffset = 2;
        for (FieldType iter : methodType.parameters())
        {
            argTypes.push_back(descriptorToType(iter, *context));
        }

        llvm::Value* callee = builder.CreateIntToPtr(builder.getInt64(lookup->getAddress()), builder.getPtrTy());
        llvm::Type* returnType = descriptorToType(methodType.returnType(), *context);

        auto* normalDest = llvm::BasicBlock::Create(*context, "", function);
        auto* exceptionDest = llvm::BasicBlock::Create(*context, "", function);
        llvm::InvokeInst* result = builder.CreateInvoke(llvm::FunctionType::get(returnType, argTypes, false), callee,
                                                        normalDest, exceptionDest, args);
        for (auto&& [index, type] : llvm::enumerate(methodType.parameters()))
        {
            auto baseType = get_if<BaseType>(&type);
            if (!baseType || !baseType->isIntegerType())
            {
                continue;
            }
            // Extend integer args for ABI.
            result->addParamAttr(parameterStartOffset + index,
                                 baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
        }

        // Require popping the local frame even if an exception is thrown.
        builder.SetInsertPoint(exceptionDest);
        // Note that the struct type used here is simply copied from Clang. The code does not make use of the struct in
        // any way except forwarding it to the resume instruction.
        llvm::LandingPadInst* landingPadInst =
            builder.CreateLandingPad(llvm::StructType::get(builder.getPtrTy(), builder.getInt32Ty()), /*NumClauses=*/0);
        // Catch all exceptions. Requires executing the resume instruction when done.
        landingPadInst->setCleanup(true);

        llvm::FunctionCallee popLocalFrame = module->getOrInsertFunction("jllvm_pop_local_frame", builder.getVoidTy());
        builder.CreateCall(popLocalFrame);
        builder.CreateResume(landingPadInst);

        builder.SetInsertPoint(normalDest);
        llvm::Value* returnValue = result;
        if (result->getType() == referenceType)
        {
            // JNI methods can only ever return a root as well. Unpack it.
            returnValue = builder.CreateLoad(referenceType, result);
        }

        builder.CreateCall(popLocalFrame);

        if (returnType->isVoidTy())
        {
            builder.CreateRetVoid();
        }
        else
        {
            builder.CreateRet(returnValue);
        }
    }
    else
    {
        // TODO: Decide whether stub should additionally attempt to load the native method on each
        // invocation or only the first.
        llvm::consumeError(lookup.takeError());
        llvm::Type* ptrType = builder.getPtrTy();

        llvm::Value* methodPtr = methodGlobal(*module, method);

        llvm::Value* exception =
            builder.CreateCall(module->getOrInsertFunction("jllvm_throw_unsatisfied_link_error",
                                                           llvm::FunctionType::get(referenceType, {ptrType}, false)),
                               {methodPtr});
        builder.CreateUnreachable();
    }

    debugInfoBuilder.finalize();

    assert(map.size() == 1 && "ByteCodeLayer only ever defines one method");
    m_irLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

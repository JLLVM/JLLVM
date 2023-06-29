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

#include "ByteCodeCompileUtils.hpp"

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

std::string jllvm::formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName,
                                     llvm::StringRef methodDescriptor)
{
    std::string result = "Java_" + escape(className) + "_" + escape(methodName);
    if (methodDescriptor.empty())
    {
        return result;
    }

    result += "__";
    // Append just the parameters from the method descriptor. This is essentially dropping the return
    // type and the parentheses.
    result += methodDescriptor.drop_front(1).take_while([](char c) { return c != ')'; });
    return result;
}

void jllvm::JNIImplementationLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                         const jllvm::MethodInfo* methodInfo, const jllvm::ClassFile* classFile,
                                         const Method* method, const ClassObject* classObject)
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

    std::string key = mangleMethod(*methodInfo, *classFile);
    llvm::cantFail(m_stubsManager->createStub(
        key,
        llvm::cantFail(m_callbackManager.getCompileCallback(
            [=, this]
            {
                // Reference:
                // https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#resolving-native-method-names
                std::string jniName = formJNIMethodName(classFile->getThisClass(), methodInfo->getName(*classFile));
                auto lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, m_interner(jniName));
                if (!lookup)
                {
                    llvm::consumeError(lookup.takeError());
                    jniName = formJNIMethodName(classFile->getThisClass(), methodInfo->getName(*classFile),
                                                methodInfo->getDescriptor(*classFile));
                    lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, m_interner(jniName));
                    if (!lookup)
                    {
                        // TODO: Return callback throwing UnsatisfiedLinkError. Don't forget to update the stubs
                        //  manager to point to that callback too!
                        llvm::report_fatal_error(lookup.takeError());
                    }
                }

                std::string bridgeName = key;

                auto context = std::make_unique<llvm::LLVMContext>();
                auto module = std::make_unique<llvm::Module>(bridgeName, *context);
                module->setDataLayout(m_dataLayout);
                module->setTargetTriple(LLVM_HOST_TRIPLE);

                llvm::DIBuilder debugBuilder(*module);
                llvm::DIFile* file = debugBuilder.createFile(".", ".");
                llvm::DICompileUnit* cu =
                    debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, "JLLVM", true, "", 0);

                llvm::DISubprogram* subprogram = debugBuilder.createFunction(
                    file, bridgeName, bridgeName, file, 1,
                    debugBuilder.createSubroutineType(debugBuilder.getOrCreateTypeArray({})), 1, llvm::DINode::FlagZero,
                    llvm::DISubprogram::SPFlagDefinition);

                MethodType methodType = parseMethodType(methodInfo->getDescriptor(*classFile));
                auto* function = llvm::Function::Create(descriptorToType(methodType, methodInfo->isStatic(), *context),
                                                        llvm::GlobalValue::ExternalLinkage, bridgeName, module.get());
                function->setSubprogram(subprogram);
                function->addFnAttr(llvm::Attribute::UWTable);

                applyJavaMethodAttributes(function, {classObject, method});

                llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                llvm::Value* environment = builder.CreateAlloca(llvm::StructType::get(builder.getPtrTy()));
                builder.CreateStore(
                    builder.CreateIntToPtr(builder.getInt64(reinterpret_cast<std::uintptr_t>(m_jniNativeFunctions)),
                                           builder.getPtrTy()),
                    environment);

                builder.CreateCall(module->getOrInsertFunction("jllvm_push_local_frame", builder.getVoidTy()));

                llvm::SmallVector<llvm::Value*> args{environment};
                if (methodInfo->isStatic())
                {
                    args.push_back(builder.CreateIntToPtr(
                        builder.getInt64(reinterpret_cast<std::uintptr_t>(classObject)), referenceType(*context)));
                }
                for (llvm::Argument& arg : function->args())
                {
                    args.push_back(&arg);
                }

                for (llvm::Value*& arg : args)
                {
                    if (arg->getType() != referenceType(*context))
                    {
                        continue;
                    }
                    arg = builder.CreateCall(
                        module->getOrInsertFunction("jllvm_new_local_root", arg->getType(), arg->getType()), arg);
                }

                llvm::Type* returnType = descriptorToType(methodType.returnType, *context);
                llvm::SmallVector<llvm::Type*> argTypes;
                // Env
                argTypes.push_back(environment->getType());
                // jclass or object
                argTypes.push_back(referenceType(*context));

                constexpr std::size_t parameterStartOffset = 2;
                for (auto& iter : methodType.parameters)
                {
                    argTypes.push_back(descriptorToType(iter, *context));
                }

                llvm::Value* callee =
                    builder.CreateIntToPtr(builder.getInt64(lookup->getAddress()), builder.getPtrTy());
                llvm::CallInst* result =
                    builder.CreateCall(llvm::FunctionType::get(returnType, argTypes, false), callee, args);
                for (auto&& [index, type] : llvm::enumerate(methodType.parameters))
                {
                    const auto* baseType = get_if<BaseType>(&type);
                    if (!baseType || !baseType->isIntegerType())
                    {
                        continue;
                    }
                    // Extend integer args for ABI.
                    result->addParamAttr(parameterStartOffset + index,
                                         baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
                }

                llvm::Value* retValue = result;
                if (retValue->getType() == referenceType(*context))
                {
                    // JNI methods can only ever return a root as well. Unpack it.
                    retValue = builder.CreateLoad(referenceType(*context), retValue);
                }

                builder.CreateCall(module->getOrInsertFunction("jllvm_pop_local_frame", builder.getVoidTy()));

                if (returnType->isVoidTy())
                {
                    builder.CreateRetVoid();
                }
                else
                {
                    builder.CreateRet(retValue);
                }

                debugBuilder.finalizeSubprogram(subprogram);
                debugBuilder.finalize();

                llvm::cantFail(
                    m_irLayer.add(m_jniBridges, llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

                llvm::JITTargetAddress bridgeMethod =
                    llvm::cantFail(m_jniBridges.getExecutionSession().lookup({&m_jniBridges}, m_interner(bridgeName)))
                        .getAddress();

                llvm::cantFail(m_stubsManager->updatePointer(key, bridgeMethod));

                return bridgeMethod;
            })),
        map.begin()->second));

    assert(map.size() == 1 && "ByteCodeLayer only ever defines one method");
    llvm::cantFail(
        mr->replace(llvm::orc::absoluteSymbols({{map.begin()->first, m_stubsManager->findStub(key, false)}})));
}

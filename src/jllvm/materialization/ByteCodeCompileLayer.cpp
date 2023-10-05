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

#include "ByteCodeCompileLayer.hpp"

#include <llvm/IR/Verifier.h>

#include "ByteCodeCompileUtils.hpp"
#include "ClassObjectStubMangling.hpp"
#include "CodeGenerator.hpp"

#define DEBUG_TYPE "jvm"

void jllvm::ByteCodeCompileLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const MethodInfo* methodInfo, const ClassFile* classFile, const Method* method,
                                       const ClassObject* classObject)
{
    std::string methodName = mangleDirectMethodCall(*methodInfo, *classFile);
    LLVM_DEBUG({ llvm::dbgs() << "Emitting LLVM IR for " << methodName << '\n'; });

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(methodName, *context);

    MethodType descriptor = methodInfo->getDescriptor(*classFile);

    auto* function = llvm::Function::Create(descriptorToType(descriptor, methodInfo->isStatic(), module->getContext()),
                                            llvm::GlobalValue::ExternalLinkage,
                                            mangleDirectMethodCall(*methodInfo, *classFile), module.get());
    function->setGC("coreclr");

    applyJavaMethodAttributes(function, {classObject, method});

    function->addFnAttr(llvm::Attribute::UWTable);
#ifdef LLVM_ADDRESS_SANITIZER_BUILD
    function->addFnAttr(llvm::Attribute::SanitizeAddress);
#endif

    auto code = methodInfo->getAttributes().find<Code>();
    assert(code);
    CodeGenerator codeGenerator{function,
                                *classFile,
                                LazyClassLoaderHelper(m_classLoader, m_mainDylib, m_stubsImplDylib, *m_stubsManager,
                                                      m_callbackManager, m_baseLayer, m_interner, m_dataLayout,
                                                      classObject, classFile),
                                m_stringInterner,
                                descriptor,
                                code->getMaxStack(),
                                code->getMaxLocals()};

    codeGenerator.generateCode(*code);

    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

#ifndef NDEBUG
    if (llvm::verifyModule(*module, &llvm::dbgs()))
    {
        std::abort();
    }
#endif

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

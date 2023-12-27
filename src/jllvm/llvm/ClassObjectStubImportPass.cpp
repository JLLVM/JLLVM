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

#include "ClassObjectStubImportPass.hpp"

#include <llvm/IR/Verifier.h>

#include <jllvm/compiler/ClassObjectStubCodeGenerator.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>

llvm::PreservedAnalyses jllvm::ClassObjectStubImportPass::run(llvm::Module& module, llvm::ModuleAnalysisManager&)
{
    ClassObject* objectClass = m_classLoader.forNameLoaded(ObjectType("java/lang/Object"));
    if (!objectClass)
    {
        return llvm::PreservedAnalyses::all();
    }

    // Early increment range as 'function' is potentially deleted.
    for (llvm::Function& function : llvm::make_early_inc_range(module.functions()))
    {
        // Only care about declarations.
        if (!function.isDeclaration())
        {
            continue;
        }
        DemangledVariant variant = demangleStubSymbolName(function.getName());
        if (holds_alternative<std::monostate>(variant))
        {
            continue;
        }

        // These optimizations are only valid and proper if the class object is already loaded.
        llvm::Function* definition = match(
            variant,
            [&](const DemangledFieldAccess& fieldAccess) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(FieldType::fromMangled(fieldAccess.className));
                if (!classObject)
                {
                    return nullptr;
                }
                return generateFieldAccessStub(module, *classObject, fieldAccess.fieldName, fieldAccess.descriptor);
            },
            [&](DemangledLoadClassObject loadClassObject) -> llvm::Function*
            {
                if (!m_classLoader.forNameLoaded(loadClassObject.classObject))
                {
                    return nullptr;
                }
                return generateClassObjectAccessStub(module, loadClassObject.classObject);
            },
            [&](const DemangledStaticCall& staticCall) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(FieldType::fromMangled(staticCall.className));
                if (!classObject)
                {
                    return nullptr;
                }
                return generateStaticCallStub(module, *classObject, staticCall.methodName, staticCall.descriptor,
                                              *objectClass);
            },
            [&](const DemangledMethodResolutionCall& methodResolutionCall) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(FieldType::fromMangled(methodResolutionCall.className));
                if (!classObject)
                {
                    return nullptr;
                }
                return generateMethodResolutionCallStub(module, methodResolutionCall.resolution, *classObject,
                                                        methodResolutionCall.methodName,
                                                        methodResolutionCall.descriptor, *objectClass);
            },
            [&](const DemangledSpecialCall& specialCall) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(FieldType::fromMangled(specialCall.className));
                if (!classObject)
                {
                    return nullptr;
                }
                ClassObject* callerClass = nullptr;
                if (specialCall.callerClass)
                {
                    callerClass = m_classLoader.forNameLoaded(*specialCall.callerClass);
                    if (!callerClass)
                    {
                        return nullptr;
                    }
                }
                return generateSpecialMethodCallStub(module, *classObject, specialCall.methodName,
                                                     specialCall.descriptor, callerClass, *objectClass);
            },
            [](...) -> llvm::Function* { return nullptr; });
        if (!definition)
        {
            continue;
        }
        // Replace the declaration with the definition and erase the declaration.
        function.replaceAllUsesWith(definition);
        function.eraseFromParent();
        // Mark it as internal linkage to avoid multiple definition errors. This also encourages LLVM to inline the
        // function.
        definition->setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    return llvm::PreservedAnalyses::none();
}

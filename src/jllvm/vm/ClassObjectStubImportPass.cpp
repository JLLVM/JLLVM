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

#include <jllvm/materialization/ClassObjectStubCodeGenerator.hpp>
#include <jllvm/materialization/ClassObjectStubMangling.hpp>

llvm::PreservedAnalyses jllvm::ClassObjectStubImportPass::run(llvm::Module& module, llvm::ModuleAnalysisManager&)
{
    ClassObject* objectClass = m_classLoader.forNameLoaded(ObjectType("java/lang/Object"));
    assert(objectClass && "java/lang/Object must have been loaded prior to any code ever executing");

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
                ClassObject* classObject = m_classLoader.forNameLoaded(ObjectType(fieldAccess.className));
                if (!classObject)
                {
                    return nullptr;
                }
                return generateFieldAccessStub(module, *classObject, fieldAccess.fieldName, fieldAccess.descriptor);
            },
            [&](FieldType fieldType) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(fieldType);
                if (!classObject)
                {
                    return nullptr;
                }
                return generateClassObjectAccessStub(module, *classObject);
            },
            [&](const DemangledStaticCall& staticCall) -> llvm::Function*
            {
                ClassObject* classObject = m_classLoader.forNameLoaded(ObjectType(staticCall.className));
                if (!classObject)
                {
                    return nullptr;
                }
                return generateStaticCallStub(module, *classObject, staticCall.methodName, staticCall.descriptor,
                                              *objectClass);
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

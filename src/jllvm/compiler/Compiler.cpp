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

#include "Compiler.hpp"

#include "ClassObjectStubMangling.hpp"
#include "CodeGenerator.hpp"

llvm::Function* jllvm::compileMethod(llvm::Module& module, const Method& method, StringInterner& stringInterner)
{
    const MethodInfo& methodInfo = method.getMethodInfo();
    const ClassObject* classObject = method.getClassObject();
    const ClassFile* classFile = classObject->getClassFile();

    MethodType descriptor = methodInfo.getDescriptor(*classFile);

    auto* function = llvm::Function::Create(descriptorToType(descriptor, methodInfo.isStatic(), module.getContext()),
                                            llvm::GlobalValue::ExternalLinkage,
                                            mangleDirectMethodCall(methodInfo, *classFile), module);
    addJavaMethodMetadata(function, {classObject, &method});
    applyABIAttributes(function);

    std::optional code = methodInfo.getAttributes().find<Code>();
    assert(code && "method to compile must have a code attribute");
    compileMethodBody(function, *classFile, *classObject, stringInterner, descriptor, *code,
                      [&](llvm::IRBuilder<>& builder, llvm::ArrayRef<llvm::AllocaInst*> locals, OperandStack&)
                      {
                          // Arguments are put into the locals. According to the specification, i64s and doubles are
                          // split into two locals. We don't actually do that, we just put them into the very first
                          // local, but we still have to skip over the following local as if we didn't.
                          auto nextLocal = locals.begin();
                          for (auto& arg : function->args())
                          {
                              builder.CreateStore(&arg, *nextLocal++);
                              if (arg.getType()->isIntegerTy(64) || arg.getType()->isDoubleTy())
                              {
                                  nextLocal++;
                              }
                          }
                      });

    return function;
}

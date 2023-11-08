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

    auto* function = llvm::Function::Create(
        descriptorToType(method.getType(), methodInfo.isStatic(), module.getContext()),
        llvm::GlobalValue::ExternalLinkage, mangleDirectMethodCall(methodInfo, *classFile), module);
    addJavaMethodMetadata(function, {classObject, &method});
    applyABIAttributes(function);

    auto* code = methodInfo.getAttributes().find<Code>();
    assert(code && "method to compile must have a code attribute");
    compileMethodBody(function, *classObject, stringInterner, method.getType(), *code,
                      [&](llvm::IRBuilder<>& builder, llvm::ArrayRef<llvm::AllocaInst*> locals, OperandStack&,
                          const ByteCodeTypeChecker::TypeInfo&)
                      {
                          // Arguments are put into the locals. According to the specification, i64s and doubles are
                          // split into two locals. We don't actually do that, we just put them into the very first
                          // local, but we still have to skip over the following local as if we didn't.
                          const auto* nextLocal = locals.begin();
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

llvm::Function* jllvm::compileOSRMethod(llvm::Module& module, std::uint16_t offset, const Method& method,
                                        StringInterner& stringInterner)
{
    const MethodInfo& methodInfo = method.getMethodInfo();
    const ClassObject* classObject = method.getClassObject();

    auto* function =
        llvm::Function::Create(osrMethodSignature(method.getType(), module.getContext()),
                               llvm::GlobalValue::ExternalLinkage, mangleOSRMethod(&method, offset), module);
    addJavaMethodMetadata(function, {classObject, &method});
    applyABIAttributes(function);

    auto* code = methodInfo.getAttributes().find<Code>();
    assert(code && "method to compile must have a code attribute");

    llvm::Value* operandStackInput = function->getArg(0);
    llvm::Value* localsInput = function->getArg(1);

    compileMethodBody(
        function, *classObject, stringInterner, method.getType(), *code,
        [&](llvm::IRBuilder<>& builder, llvm::ArrayRef<llvm::AllocaInst*> locals, OperandStack& operandStack,
            const ByteCodeTypeChecker::TypeInfo& typeInfo)
        {
            // Initialize the operand stack and the local variables from the two input arrays. Using the type info from
            // the type checker, it is possible to load the exact types required.
            for (auto&& [index, type] : llvm::enumerate(typeInfo.operandStack))
            {
                assert(type.is<llvm::Type*>()
                       && "OSR into frame containing 'returnAddress' instances is not supported");
                llvm::Value* gep = builder.CreateConstGEP1_32(operandStackInput->getType(), operandStackInput, index);
                llvm::Value* load = builder.CreateLoad(type.get<llvm::Type*>(), gep);
                operandStack.push_back(load);
            }

            for (auto&& [index, pair] : llvm::enumerate(llvm::zip(typeInfo.locals, locals)))
            {
                auto&& [type, local] = pair;
                // If the local variable has no type, it is uninitialized at that point in time. There is no code for us
                // to generate in this case.
                if (!type)
                {
                    continue;
                }
                assert(type.is<llvm::Type*>()
                       && "OSR into frame containing 'returnAddress' instances is not supported");
                llvm::Value* gep = builder.CreateConstGEP1_32(localsInput->getType(), localsInput, index);
                llvm::Value* load = builder.CreateLoad(type.get<llvm::Type*>(), gep);
                builder.CreateStore(load, local);
            }

            // The OSR frame is responsible for deleting its input arrays as the frame that originally allocated the
            // pointer is replaced.
            builder.CreateCall(function->getParent()->getOrInsertFunction("jllvm_osr_frame_delete", builder.getVoidTy(),
                                                                          builder.getPtrTy()),
                               operandStackInput);
        },
        offset);

    return function;
}

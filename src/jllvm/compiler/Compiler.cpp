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

llvm::Function* jllvm::compileMethod(llvm::Module& module, const Method& method)
{
    const MethodInfo& methodInfo = method.getMethodInfo();
    const ClassObject* classObject = method.getClassObject();
    const ClassFile* classFile = classObject->getClassFile();

    auto* function = llvm::Function::Create(
        descriptorToType(method.getType(), methodInfo.isStatic(), module.getContext()),
        llvm::GlobalValue::ExternalLinkage, mangleDirectMethodCall(methodInfo, *classFile), module);
    addJavaMethodMetadata(function, &method, JavaMethodMetadata::Kind::JIT);
    applyABIAttributes(function);

    auto* code = methodInfo.getAttributes().find<Code>();
    assert(code && "method to compile must have a code attribute");
    compileMethodBody(
        function, method, *code,
        [&](llvm::IRBuilder<>& builder, LocalVariables& locals, OperandStack&, const ByteCodeTypeChecker::TypeInfo&)
        {
            // Arguments are put into the locals. According to the specification, i64s and doubles are
            // split into two locals. We don't actually do that, we just put them into the very first
            // local, but we still have to skip over the following local as if we didn't.
            auto nextLocal = locals.begin();
            std::size_t functionArgsBegin = 0;
            if (!method.isStatic())
            {
                functionArgsBegin = 1;
                *nextLocal++ = function->getArg(0);
            }
            for (auto&& [arg, paramType] :
                 llvm::zip_equal(llvm::drop_begin(function->args(), functionArgsBegin), method.getType().parameters()))
            {
                *nextLocal++ = extendToStackType(builder, paramType, &arg);
                if (paramType.isWide())
                {
                    nextLocal++;
                }
            }
        });

    return function;
}

llvm::Function* jllvm::compileOSRMethod(llvm::Module& module, std::uint16_t offset, const Method& method)
{
    const MethodInfo& methodInfo = method.getMethodInfo();

    auto* function =
        llvm::Function::Create(osrMethodSignature(method.getType(), module.getContext()),
                               llvm::GlobalValue::ExternalLinkage, mangleOSRMethod(&method, offset), module);
    addJavaMethodMetadata(function, &method, JavaMethodMetadata::Kind::JIT);
    applyABIAttributes(function);

    auto* code = methodInfo.getAttributes().find<Code>();
    assert(code && "method to compile must have a code attribute");

    llvm::Value* osrState = function->getArg(0);

    compileMethodBody(
        function, method, *code,
        [&](llvm::IRBuilder<>& builder, LocalVariables& locals, OperandStack& operandStack,
            const ByteCodeTypeChecker::TypeInfo& typeInfo)
        {
            // Initialize the operand stack and the local variables from the two input arrays. Using the type info from
            // the type checker, it is possible to load the exact types required.
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
                llvm::Value* gep = builder.CreateConstGEP1_32(builder.getInt64Ty(), osrState, index);
                llvm::Value* load = builder.CreateLoad(type.get<llvm::Type*>(), gep);
                local = load;
            }

            {
                std::size_t index = 0;
                for (ByteCodeTypeChecker::JVMType type : typeInfo.operandStack)
                {
                    assert(type.is<llvm::Type*>()
                           && "OSR into frame containing 'returnAddress' instances is not supported");
                    llvm::Value* gep =
                        builder.CreateConstGEP1_32(builder.getInt64Ty(), osrState, index + typeInfo.locals.size());
                    index++;
                    auto* llvmType = type.get<llvm::Type*>();
                    llvm::Value* load = builder.CreateLoad(llvmType, gep);
                    operandStack.push_back(load);
                    // Double and long take up two operand stack slots. Skip over the second. The first already
                    // contained the value.
                    if (llvmType->isDoubleTy() || llvmType->isIntegerTy(64))
                    {
                        index++;
                        continue;
                    }
                }
            }

            // The OSR frame is responsible for deleting its input arrays as the frame that originally allocated the
            // pointer is replaced.
            llvm::FunctionCallee callee = function->getParent()->getOrInsertFunction(
                "jllvm_osr_frame_delete", builder.getVoidTy(), builder.getPtrTy());
            llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr("gc-leaf-function");
            builder.CreateCall(callee, osrState);
        },
        offset);

    return function;
}

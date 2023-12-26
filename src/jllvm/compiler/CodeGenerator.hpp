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

#pragma once

#include <llvm/IR/DIBuilder.h>

#include <jllvm/class/ByteCodeIterator.hpp>

#include <map>

#include "CodeGeneratorUtils.hpp"

namespace jllvm
{
/// Class for generating LLVM IR from Java Bytecode
/// This class should ideally be used to generate code for a single method
class CodeGenerator
{
    struct BasicBlockData
    {
        llvm::BasicBlock* block;
        OperandStack::State operandState;
        LocalVariables::State variableState;
    };

    llvm::Function* m_function;
    const Method& m_method;
    const ClassObject& m_classObject;
    const ClassFile& m_classFile;
    const Code& m_code;
    llvm::IRBuilder<> m_builder;
    OperandStack m_operandStack;
    LocalVariables m_locals;
    llvm::DenseMap<std::uint16_t, BasicBlockData> m_basicBlocks;
    ByteCodeTypeChecker::PossibleRetsMap m_retToMap;
    llvm::SmallSetVector<std::uint16_t, 8> m_workList;

    /// Returns the basic block corresponding to the given bytecode offset and schedules the basic block to be compiled.
    /// The offset must point to the start of a basic block.
    llvm::BasicBlock* getBasicBlock(std::uint16_t offset)
    {
        m_workList.insert(offset);
        return m_basicBlocks.find(offset)->second.block;
    }

    void createBasicBlocks(const ByteCodeTypeChecker& checker);

    void generateCodeBody(std::uint16_t startOffset);

    /// Generate LLVM IR instructions for a JVM bytecode instruction. Returns true if the instruction falls through or
    /// more formally, whether the next instruction is an immediate successor of this instruction.
    bool generateInstruction(ByteCodeOp operation);

    /// Creates a new call from 'callInst' which contains the deoptimization information required for exception
    /// handling. The deoptimization information can be read by the unwinder using the stackmap and is of the form:
    ///     uint16_t byteCodeOffset;
    ///     uint16_t numLocals;
    ///     locations locals[numLocals];
    ///
    /// The original call is replaced and erased.
    void addExceptionHandlingDeopts(std::uint16_t byteCodeOffset, llvm::CallBase*& callInst);

    /// Creates a new call from 'callInst' which contains the deoptimization information required for generating a Java
    /// backtrace. This is equal to using 'addExceptionHandlingDeopts' but with the number of local variables set to
    /// zero.
    void addBytecodeOffsetOnlyDeopts(std::uint16_t byteCodeOffset, llvm::CallBase*& callInst);

    void generateBuiltinExceptionThrow(std::uint16_t byteCodeOffset, llvm::Value* condition,
                                       llvm::StringRef builderName, llvm::ArrayRef<llvm::Value*> builderArgs);

    void generateNullPointerCheck(std::uint16_t byteCodeOffset, llvm::Value* object);

    void generateArrayIndexCheck(std::uint16_t byteCodeOffset, llvm::Value* array, llvm::Value* index);

    void generateNegativeArraySizeCheck(std::uint16_t byteCodeOffset, llvm::Value* size);

    void generateExceptionThrow(std::uint16_t byteCodeOffset, llvm::Value* exception);

    llvm::Value* loadClassObjectFromPool(std::uint16_t offset, PoolIndex<ClassInfo> index);

    llvm::Value* generateAllocArray(std::uint16_t offset, ArrayType descriptor, llvm::Value* classObject,
                                    llvm::Value* size);

    /// Creates a non-virtual call to the static function 'methodName' of the type 'methodType' within
    /// 'className' using 'args'. This is used to implement `invokestatic`.
    llvm::Value* doStaticCall(std::uint16_t offset, llvm::StringRef className, llvm::StringRef methodName,
                              MethodType methodType, llvm::ArrayRef<llvm::Value*> args);

    /// Creates a virtual call to the function 'methodName' of the type 'methodType' within 'className' using 'args'.
    /// 'resolution' determines how the actual method to be called is resolved using the previously mentioned strings.
    llvm::Value* doInstanceCall(std::uint16_t offset, llvm::StringRef className, llvm::StringRef methodName,
                                MethodType methodType, llvm::ArrayRef<llvm::Value*> args, MethodResolution resolution);

    /// Creates an 'invokespecial' call to the function 'methodName' of the type 'methodType' within 'className' using
    /// 'args'.
    llvm::Value* doSpecialCall(std::uint16_t offset, llvm::StringRef className, llvm::StringRef methodName,
                               MethodType methodType, llvm::ArrayRef<llvm::Value*> args);

    /// Returns an LLVM integer constant which contains the offset of the 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getInstanceFieldOffset(std::uint16_t offset, llvm::StringRef className, llvm::StringRef fieldName,
                                        FieldType fieldType);

    /// Returns an LLVM Pointer which points to the static field 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getStaticFieldAddress(std::uint16_t offset, llvm::StringRef className, llvm::StringRef fieldName,
                                       FieldType fieldType);

    /// Returns an LLVM Pointer which points to the class object of the type with the given field descriptor.
    llvm::Value* getClassObject(std::uint16_t offset, FieldType fieldDescriptor);

public:
    CodeGenerator(llvm::Function* function, const Method& method)
        : m_function{function},
          m_method{method},
          m_classObject{*method.getClassObject()},
          m_classFile{*m_classObject.getClassFile()},
          m_code{*m_method.getMethodInfo().getAttributes().find<Code>()},
          m_builder{llvm::BasicBlock::Create(function->getContext(), "entry", function)},
          m_operandStack{m_builder, m_code.getMaxStack()},
          m_locals{m_builder, m_code.getMaxLocals()}
    {
    }

    using PrologueGenFn =
        llvm::function_ref<void(llvm::IRBuilder<>& builder, LocalVariables& locals, OperandStack& operandStack,
                                const ByteCodeTypeChecker::TypeInfo& typeInfo)>;

    /// This function must be only called once. 'generatePrologue' is used to initialize the local variables and
    /// operand stack at the start of the method. 'offset' is the bytecode offset at which compilation should start and
    /// must refer to a JVM instruction.
    void generateBody(PrologueGenFn generatePrologue, std::uint16_t offset = 0);
};

/// Generates new LLVM code at the back of 'function' from the JVM Bytecode in 'method'.
/// 'generatePrologue' is called by the function to initialize the operand stack and local variables at the beginning of
/// the newly created code. 'offset' is the bytecode offset at which compilation should start and must refer to a JVM
/// instruction.
inline void compileMethodBody(llvm::Function* function, const Method& method,
                              CodeGenerator::PrologueGenFn generatePrologue, std::uint16_t offset = 0)
{
    CodeGenerator codeGenerator{function, method};

    codeGenerator.generateBody(generatePrologue, offset);
}

} // namespace jllvm

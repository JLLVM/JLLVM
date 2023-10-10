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
#include <jllvm/object/StringInterner.hpp>

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
        OperandStack::State state;
    };

    llvm::Function* m_function;
    const ClassFile& m_classFile;
    const ClassObject& m_classObject;
    StringInterner& m_stringInterner;
    MethodType m_functionMethodType;
    llvm::IRBuilder<> m_builder;
    llvm::DIBuilder m_debugBuilder;
    OperandStack m_operandStack;
    std::vector<llvm::AllocaInst*> m_locals;
    llvm::DenseMap<std::uint16_t, BasicBlockData> m_basicBlocks;
    ByteCodeTypeChecker::PossibleRetsMap m_retToMap;

    using HandlerInfo = std::pair<std::uint16_t, PoolIndex<ClassInfo>>;

    // std::list because we want the iterator stability when deleting handlers (requires random access).
    std::list<HandlerInfo> m_activeHandlers;
    // std::map because it is the easiest to use with std::list key.
    std::map<std::list<HandlerInfo>, llvm::BasicBlock*> m_alreadyGeneratedHandlers;

    void createBasicBlocks(const Code& code);

    void generateCodeBody(const Code& code);

    /// Generate LLVM IR instructions for a JVM bytecode instruction, returns whether the instruction indicated the end
    /// of a basic block
    bool generateInstruction(ByteCodeOp operation);

    void generateEHDispatch();

    void generateBuiltinExceptionThrow(llvm::Value* condition, llvm::StringRef builderName,
                                       llvm::ArrayRef<llvm::Value*> builderArgs);

    void generateNullPointerCheck(llvm::Value* object);

    void generateArrayIndexCheck(llvm::Value* array, llvm::Value* index);

    void generateNegativeArraySizeCheck(llvm::Value* size);

    llvm::BasicBlock* generateHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred);

    llvm::Value* loadClassObjectFromPool(PoolIndex<ClassInfo> index);

    llvm::Value* generateAllocArray(ArrayType descriptor, llvm::Value* classObject, llvm::Value* size);

    /// Creates a non-virtual call to the static function 'methodName' of the type 'methodType' within
    /// 'className' using 'args'. This is used to implement `invokestatic`.
    llvm::Value* doStaticCall(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef methodName,
                              MethodType methodType, llvm::ArrayRef<llvm::Value*> args);

    /// Creates a virtual call to the function 'methodName' of the type 'methodType' within 'className' using 'args'.
    /// 'resolution' determines how the actual method to be called is resolved using the previously mentioned strings.
    llvm::Value* doInstanceCall(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef methodName,
                                MethodType methodType, llvm::ArrayRef<llvm::Value*> args, MethodResolution resolution);

    /// Creates an 'invokespecial' call to the function 'methodName' of the type 'methodType' within 'className' using
    /// 'args'.
    llvm::Value* doSpecialCall(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef methodName,
                               MethodType methodType, llvm::ArrayRef<llvm::Value*> args);

    /// Returns an LLVM integer constant which contains the offset of the 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getInstanceFieldOffset(llvm::IRBuilder<>& builder, llvm::StringRef className,
                                        llvm::StringRef fieldName, FieldType fieldType);

    /// Returns an LLVM Pointer which points to the static field 'fieldName' with the type 'fieldType'
    /// within the class 'className'.
    llvm::Value* getStaticFieldAddress(llvm::IRBuilder<>& builder, llvm::StringRef className, llvm::StringRef fieldName,
                                       FieldType fieldType);

    /// Returns an LLVM Pointer which points to the class object of the type with the given field descriptor.
    llvm::Value* getClassObject(llvm::IRBuilder<>& builder, FieldType fieldDescriptor);

public:
    CodeGenerator(llvm::Function* function, const ClassFile& classFile, const ClassObject& classObject,
                  StringInterner& stringInterner, MethodType methodType, std::uint16_t maxStack,
                  std::uint16_t maxLocals)
        : m_function{function},
          m_classFile{classFile},
          m_classObject(classObject),
          m_stringInterner{stringInterner},
          m_functionMethodType{methodType},
          m_builder{llvm::BasicBlock::Create(function->getContext(), "entry", function)},
          m_debugBuilder{*function->getParent()},
          m_operandStack{m_builder, maxStack},
          m_locals{maxLocals}
    {
    }

    /// This function must be only called once. 'code' must have at most a maximum stack depth of 'maxStack'
    /// and have at most 'maxLocals' local variables.
    void generateCode(const Code& code);
};
} // namespace jllvm

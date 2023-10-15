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
    LazyClassLoaderHelper m_helper;
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

    void generateNullPointerCheck(llvm::Value* object);

    void generateArrayIndexCheck(llvm::Value* array, llvm::Value* index);

    llvm::BasicBlock* generateHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred);

    llvm::Value* loadClassObjectFromPool(PoolIndex<ClassInfo> index);

    llvm::Value* generateAllocArray(ArrayType descriptor, llvm::Value* classObject, llvm::Value* size);

public:
    CodeGenerator(llvm::Function* function, const ClassFile& classFile, LazyClassLoaderHelper helper,
                  StringInterner& stringInterner, MethodType methodType, std::uint16_t maxStack,
                  std::uint16_t maxLocals)
        : m_function{function},
          m_classFile{classFile},
          m_helper{std::move(helper)},
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

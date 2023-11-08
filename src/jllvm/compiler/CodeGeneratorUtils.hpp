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

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <jllvm/class/ByteCodeIterator.hpp>
#include <jllvm/object/ClassLoader.hpp>

#include "ByteCodeCompileUtils.hpp"
#include "ClassObjectStubMangling.hpp"

namespace jllvm
{

/// Class for Java bytecode typechecking
/// This works by iterating over the bytecode of a Java method extracting the basic blocks
/// and the types on the stack at the start of the block then constructing a map of basic block starting
/// offsets to starting state of their stack.
class ByteCodeTypeChecker
{
    struct ReturnInfo
    {
        std::uint16_t retOffset;
        std::uint16_t returnAddress;
    };

public:
    using RetAddrType = llvm::PointerEmbeddedInt<std::uint16_t>;
    using JVMType = llvm::PointerUnion<llvm::Type*, RetAddrType>;
    using TypeStack = std::vector<JVMType>;
    using BasicBlockMap = llvm::DenseMap<std::uint16_t, TypeStack>;
    using PossibleRetsMap = llvm::DenseMap<std::uint16_t, llvm::DenseSet<std::uint16_t>>;

    /// Point in the 'ByteCodeTypeChecker' where the local variable and operand stack types should be extracted.
    /// A local variable may be null in which case the local variable is currently uninitialized.
    struct TypeInfo
    {
        std::uint16_t offset{};
        std::vector<JVMType> operandStack;
        std::vector<JVMType> locals;
    };

private:
    llvm::LLVMContext& m_context;
    const ClassFile& m_classFile;
    const Code& m_code;
    std::vector<std::uint16_t> m_offsetStack;
    std::vector<JVMType> m_locals;
    llvm::DenseMap<std::uint16_t, std::uint16_t> m_returnAddressToSubroutineMap;
    llvm::DenseMap<std::uint16_t, ReturnInfo> m_subroutineToReturnInfoMap;
    BasicBlockMap m_basicBlocks;
    llvm::Type* m_addressType;
    llvm::Type* m_doubleType;
    llvm::Type* m_floatType;
    llvm::Type* m_intType;
    llvm::Type* m_longType;
    TypeInfo m_byteCodeTypeInfo;

    void checkBasicBlock(llvm::ArrayRef<char> block, std::uint16_t offset, TypeStack typeStack);

public:
    ByteCodeTypeChecker(llvm::LLVMContext& context, const ClassFile& classFile, const Code& code, MethodType methodType)
        : m_context{context},
          m_classFile{classFile},
          m_code{code},
          m_locals(code.getMaxLocals()),
          m_addressType{referenceType(m_context)},
          m_doubleType{llvm::Type::getDoubleTy(m_context)},
          m_floatType{llvm::Type::getFloatTy(m_context)},
          m_intType{llvm::Type::getInt32Ty(m_context)},
          m_longType{llvm::Type::getInt64Ty(m_context)}
    {
        // Types of local variables at method entry are the arguments of the parameters.
        for (auto&& [paramType, local] : llvm::zip(methodType.parameters(), m_locals))
        {
            local = descriptorToType(paramType, context);
        }
    }

    /// Type-checks the entire java method, returning the 'ByteCodeTypeInfo' for the instruction at 'offset'.
    const TypeInfo& checkAndGetTypeInfo(std::uint16_t offset);

    /// Creates a mapping between each 'ret' instruction and the offsets inside the bytecode where it could return to.
    PossibleRetsMap makeRetToMap() const;

    const BasicBlockMap& getBasicBlocks() const
    {
        return m_basicBlocks;
    }
};

/// Class for JVM operand stack
/// This class also offers method to save and restore the current state of the stack in order to consider the control
/// flow path
class OperandStack
{
    std::vector<llvm::AllocaInst*> m_values;
    std::vector<llvm::Type*> m_types;
    llvm::IRBuilder<>& m_builder;
    std::size_t m_topOfStack{};

public:
    using State = std::vector<llvm::Type*>;

    OperandStack(llvm::IRBuilder<>& builder, std::uint16_t maxStack)
        : m_builder(builder), m_values{maxStack}, m_types{maxStack}
    {
        std::generate(m_values.begin(), m_values.end(), [&] { return builder.CreateAlloca(builder.getPtrTy()); });
    }

    llvm::Value* pop_back()
    {
        llvm::AllocaInst* alloc = m_values[--m_topOfStack];
        llvm::Type* type = m_types[m_topOfStack];
        return m_builder.CreateLoad(type, alloc);
    }

    std::pair<llvm::Value*, llvm::Type*> pop_back_with_type()
    {
        llvm::AllocaInst* alloc = m_values[--m_topOfStack];
        llvm::Type* type = m_types[m_topOfStack];

        return {m_builder.CreateLoad(type, alloc), type};
    }

    void push_back(llvm::Value* value)
    {
        llvm::AllocaInst* alloc = m_values[m_topOfStack];
        m_types[m_topOfStack++] = value->getType();
        m_builder.CreateStore(value, alloc);
    }

    void setState(const State& state)
    {
        llvm::copy(state, m_types.begin());
        m_topOfStack = state.size();
    }

    /// Sets the value of the bottom-most stack slot of the operand stack.
    void setBottomOfStackValue(llvm::Value* value) const
    {
        m_builder.CreateStore(value, m_values.front());
    }
};

} // namespace jllvm

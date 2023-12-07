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

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{

/// Generates LLVM IR using 'builder' creating the required state and instructions followed by the function call to
/// execute 'method' in the interpreter. 'generatePrologue' is used to initialize the abstract machine state which are
/// initially uninitialized. Returns the result of the interpreter as the corresponding LLVM type of the return type of
/// 'method'.
llvm::Value* generateInterpreterEntry(
    llvm::IRBuilder<>& builder, const Method& method,
    llvm::function_ref<void(llvm::IRBuilder<>& builder, llvm::AllocaInst* byteCodeOffset, llvm::AllocaInst* topOfStack,
                            llvm::AllocaInst* operandStack, llvm::AllocaInst* operandGCMask,
                            llvm::AllocaInst* localVariables, llvm::AllocaInst* localVariablesGCMask, const Code& code)>
        generatePrologue);
} // namespace jllvm

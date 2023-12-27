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

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/object/ClassObject.hpp>

#include "ByteCodeCompileUtils.hpp"

namespace jllvm
{

/// Compiles 'method' to a new LLVM function inside of 'module' and returns it.
llvm::Function* compileMethod(llvm::Module& module, const Method& method);

/// Compiles 'method' to a LLVM function suitable for OSR entry at the bytecode offset 'offset'. The function is placed
/// into 'module' and returned. The return type of the function is suitable for replacing the method with the given
/// calling convention.
llvm::Function* compileOSRMethod(llvm::Module& module, std::uint16_t offset, const Method& method,
                                 CallingConvention callingConvention);
} // namespace jllvm

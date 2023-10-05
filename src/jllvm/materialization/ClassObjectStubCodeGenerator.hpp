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

#include <llvm/IR/Module.h>

#include <jllvm/object/ClassObject.hpp>

#include "ClassObjectStubMangling.hpp"

/// These functions contain the logic to generate LLVM functions implementing the definitions of the stubs returned by
/// the mangling functions in 'ClassObjectStubMangling.hpp'. This may be used either by the JIT to on-demand compile
/// the definitions of these functions, or by the LLVM optimization pipeline to import these function definitions into
/// a module.
namespace jllvm
{

/// Generates a new LLVM function with the name returned by 'mangleFieldAccess' returning either the address of the
/// static field or the byte offset of an instance field of the field given by 'fieldName' and 'descriptor' inside of
/// 'classObject'.
/// It is undefined behaviour if the field does not exist.
llvm::Function* generateFieldAccessStub(llvm::Module& module, const ClassObject& classObject, llvm::StringRef fieldName,
                                        FieldType descriptor);

llvm::Function* generateMethodResolutionCallStub(llvm::Module& module, MethodResolution resolution,
                                                 const ClassObject& classObject, llvm::StringRef fieldName,
                                                 MethodType descriptor);

/// Generates a new LLVM function with the name returned by 'mangleStaticCall' implementing the method resolution and
/// method selection of a static call before then calling the found method.
/// It is undefined behaviour if method resolution does not find a method to call.
llvm::Function* generateStaticCallStub(llvm::Module& module, const ClassObject& classObject, llvm::StringRef methodName,
                                       MethodType descriptor, const ClassObject& objectClass);

llvm::Function* generateClassObjectAccessStub(llvm::Module& module, const ClassObject& classObject);

} // namespace jllvm

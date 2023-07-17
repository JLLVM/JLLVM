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

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <jllvm/class/Descriptors.hpp>
#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{
/// Returns the pointer type used by the JVM for arrays of references.
llvm::Type* arrayRefType(llvm::LLVMContext& context);

/// Returns the pointer type used for any Java array types storing elements of 'elementType'.
llvm::Type* arrayStructType(llvm::Type* elementType);

/// Returns the pointer type used by the JVM for interface tables.
llvm::Type* iTableType(llvm::LLVMContext& context);

/// Returns the pointer type used by the JVM for object headers.
llvm::Type* objectHeaderType(llvm::LLVMContext& context);

/// Returns the pointer type used for all Java reference types.
/// This is a pointer tagged with an address space for the sake of the GC.
llvm::PointerType* referenceType(llvm::LLVMContext& context);

/// Returns the corresponding LLVM type for a given Java field descriptor.
llvm::Type* descriptorToType(FieldType type, llvm::LLVMContext& context);

/// Returns the corresponding LLVM function type for a given, possible static, Java method descriptor.
llvm::FunctionType* descriptorToType(MethodType type, bool isStatic, llvm::LLVMContext& context);

/// Metadata attached to Java methods produced by any 'ByteCodeLayer' implementation.
struct JavaMethodMetadata
{
    /// Class object of the enclosing class of the method.
    const ClassObject* classObject;
    /// Method meta-object of the compiled method.
    const Method* method;
};

void applyJavaMethodAttributes(llvm::Function* function, const JavaMethodMetadata& metadata);
} // namespace jllvm

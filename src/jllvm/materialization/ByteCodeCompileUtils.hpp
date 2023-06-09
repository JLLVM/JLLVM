#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <jllvm/class/Descriptors.hpp>

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
llvm::Type* descriptorToType(const FieldType& type, llvm::LLVMContext& context);

/// Returns the corresponding LLVM function type for a given, possible static, Java method descriptor.
llvm::FunctionType* descriptorToType(const MethodType& type, bool isStatic, llvm::LLVMContext& context);
} // namespace jllvm

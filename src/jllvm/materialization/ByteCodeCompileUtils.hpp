#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <jllvm/class/Descriptors.hpp>

namespace jllvm
{
/// Returns the pointer type used for any Java reference types. This is a pointer tagged with an address space for the
/// sake of the GC.
llvm::PointerType* referenceType(llvm::LLVMContext& context);

/// Returns the corresponding LLVM type for a given Java field descriptor.
llvm::Type* descriptorToType(const FieldType& type, llvm::LLVMContext& context);

/// Returns the corresponding LLVM function type for a given, possible static, Java method descriptor.
llvm::FunctionType* descriptorToType(const MethodType& type, bool isStatic, llvm::LLVMContext& context);
} // namespace jllvm

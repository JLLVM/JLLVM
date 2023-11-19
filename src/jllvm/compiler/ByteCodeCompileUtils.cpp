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

#include "ByteCodeCompileUtils.hpp"

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <jllvm/support/Variant.hpp>

#include "ClassObjectStubMangling.hpp"

llvm::Type* jllvm::arrayRefType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(context, {llvm::PointerType::get(context, 0),
                                           llvm::Type::getIntNTy(context, std::numeric_limits<std::size_t>::digits)});
}

llvm::Type* jllvm::arrayStructType(llvm::Type* elementType)
{
    return llvm::StructType::get(elementType->getContext(), {objectHeaderType(elementType->getContext()),
                                                             llvm::Type::getInt32Ty(elementType->getContext()),
                                                             llvm::ArrayType::get(elementType, 0)});
}

llvm::Type* jllvm::iTableType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(context, {llvm::Type::getIntNTy(context, std::numeric_limits<std::size_t>::digits),
                                           llvm::ArrayType::get(llvm::PointerType::get(context, 0), 0)});
}

llvm::Type* jllvm::objectHeaderType(llvm::LLVMContext& context)
{
    return llvm::StructType::get(/*classObject*/ jllvm::referenceType(context),
                                 /*hashCode*/ llvm::Type::getInt32Ty(context));
}

llvm::PointerType* jllvm::referenceType(llvm::LLVMContext& context)
{
    return llvm::PointerType::get(context, 1);
}

namespace
{
/// Get or insert a global of the given 'name' in 'module' which has external linkage and simply imports the symbol
/// 'name'.
llvm::GlobalVariable* getOrInsertImportingGlobal(llvm::Module& module, llvm::StringRef name, unsigned addressSpace)
{
    if (llvm::GlobalVariable* variable = module.getGlobalVariable(name))
    {
        return variable;
    }

    // The actual storage type given here is irrelevant as LLVM makes no assumptions about the size and actual type of
    // external globals.
    auto* storageType = llvm::IntegerType::get(module.getContext(), 8);
    return new llvm::GlobalVariable(module, storageType, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
                                    /*Initializer=*/nullptr, name, nullptr, llvm::GlobalValue::NotThreadLocal,
                                    addressSpace,
                                    /*isExternallyInitialized=*/true);
}
} // namespace

llvm::GlobalVariable* jllvm::classObjectGlobal(llvm::Module& module, FieldType classObject)
{
    return getOrInsertImportingGlobal(module, mangleClassObjectGlobal(classObject), /*addressSpace=*/1);
}

llvm::GlobalVariable* jllvm::methodGlobal(llvm::Module& module, const Method* method)
{
    return getOrInsertImportingGlobal(module, mangleMethodGlobal(method), /*addressSpace=*/0);
}

llvm::Type* jllvm::descriptorToType(FieldType type, llvm::LLVMContext& context)
{
    return jllvm::match(
        type,
        [&](BaseType baseType) -> llvm::Type*
        {
            switch (baseType.getValue())
            {
                case jllvm::BaseType::Void: return llvm::Type::getVoidTy(context);
                case BaseType::Boolean:
                case BaseType::Byte: return llvm::Type::getInt8Ty(context);
                case BaseType::Short:
                case BaseType::Char: return llvm::Type::getInt16Ty(context);
                case BaseType::Double: return llvm::Type::getDoubleTy(context);
                case BaseType::Float: return llvm::Type::getFloatTy(context);
                case BaseType::Int: return llvm::Type::getInt32Ty(context);
                case BaseType::Long: return llvm::Type::getInt64Ty(context);
            }
            llvm_unreachable("Invalid type");
        },
        [&](const ArrayType&) -> llvm::Type* { return referenceType(context); },
        [&](ObjectType) -> llvm::Type* { return referenceType(context); });
}

llvm::FunctionType* jllvm::descriptorToType(MethodType type, bool isStatic, llvm::LLVMContext& context)
{
    auto args = llvm::to_vector(
        llvm::map_range(type.parameters(), [&](const auto& elem) { return descriptorToType(elem, context); }));
    if (!isStatic)
    {
        args.insert(args.begin(), referenceType(context));
    }
    return llvm::FunctionType::get(descriptorToType(type.returnType(), context), args, false);
}

void jllvm::addJavaMethodMetadata(llvm::Function* function, const JavaMethodMetadata& metadata)
{
    std::string sectionName = "java";
    if (llvm::Triple(LLVM_HOST_TRIPLE).isOSBinFormatMachO())
    {
        sectionName = "__TEXT," + sectionName;
        sectionName += ",regular,pure_instructions";
    }

    auto* ptrType = llvm::PointerType::get(function->getContext(), 0);
    function->setPrefixData(llvm::ConstantStruct::get(
        llvm::StructType::get(function->getContext(), {referenceType(function->getContext()), ptrType}),
        {classObjectGlobal(*function->getParent(), metadata.classObject->getDescriptor()),
         methodGlobal(*function->getParent(), metadata.method)}));
    function->setSection(sectionName);
}

namespace
{
using namespace jllvm;

/// X86 ABI essentially always uses the 32 bit register names for passing along integers. Using the 'signext' and
/// 'zeroext' attribute we tell LLVM that if due to ABI, it has to extend these registers, which extension to use.
/// This attribute list can be applied to either a call or a function itself.
llvm::AttributeList getABIAttributes(llvm::LLVMContext& context, MethodType methodType, bool isStatic)
{
    llvm::SmallVector<llvm::AttributeSet> paramAttrs(methodType.size());
    for (auto&& [param, attrs] : llvm::zip(methodType.parameters(), paramAttrs))
    {
        auto baseType = get_if<BaseType>(&param);
        if (!baseType || !baseType->isIntegerType())
        {
            continue;
        }
        attrs = attrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }

    llvm::AttributeSet retAttrs;
    FieldType returnType = methodType.returnType();
    if (auto baseType = get_if<BaseType>(&returnType); baseType && baseType->isIntegerType())
    {
        retAttrs =
            retAttrs.addAttribute(context, baseType->isUnsigned() ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
    }
    if (!isStatic)
    {
        paramAttrs.insert(paramAttrs.begin(), llvm::AttributeSet().addAttribute(context, llvm::Attribute::NonNull));
    }
    return llvm::AttributeList::get(context, llvm::AttributeSet{}, retAttrs, paramAttrs);
}

} // namespace

void jllvm::applyABIAttributes(llvm::Function* function, MethodType methodType, bool isStatic)
{
    llvm::AttributeList attributeList = getABIAttributes(function->getContext(), methodType, isStatic);
    // The RS4GC pass creating the `gc.statepoint` intrinsics that we currently use do not support `signext` and
    // `zeroext` argument attributes. These are important as they lead to either zero or sign extending an integer
    // register in the caller to 32 bit, something that is expected by basically all C ABIs.
    // To circumvent this, be conservative and remove the attribute from all function parameters. This makes the
    // function assume the caller did not extend the integer. This makes the function compatible with both a caller from
    // C++ code, which does the extension properly, and a caller from Java code which does not.
    // Calling C code only occurs in the JNI where the bridge does not need GC instrumentation and does the extension
    // correctly.
    // TODO: Remove this once we are using LLVM 18 where RS4GC does not discard `signext` and `zeroext`
    //       or we use a local fork of the RS4GC pass that copies these.
    //       See https://github.com/llvm/llvm-project/pull/68475 and https://github.com/llvm/llvm-project/pull/68439.
    for (std::size_t i : llvm::seq<std::size_t>(0, function->arg_size()))
    {
        attributeList = attributeList.removeParamAttribute(function->getContext(), i, llvm::Attribute::ZExt);
        attributeList = attributeList.removeParamAttribute(function->getContext(), i, llvm::Attribute::SExt);
    }
    function->setAttributes(attributeList);
    applyABIAttributes(function);
}

void jllvm::applyABIAttributes(llvm::Function* function)
{
#ifdef LLVM_ADDRESS_SANITIZER_BUILD
    function->addFnAttr(llvm::Attribute::SanitizeAddress);
#endif
    function->addFnAttr(llvm::Attribute::getWithUWTableKind(function->getContext(), llvm::UWTableKind::Async));
    function->setGC("coreclr");
}

void jllvm::applyABIAttributes(llvm::CallBase* call, MethodType methodType, bool isStatic)
{
    call->setAttributes(getABIAttributes(call->getContext(), methodType, isStatic));
}

llvm::FunctionType* jllvm::osrMethodSignature(MethodType methodType, llvm::LLVMContext& context)
{
    auto* pointerType = llvm::PointerType::get(context, 0);
    return llvm::FunctionType::get(descriptorToType(methodType.returnType(), context), {pointerType, pointerType},
                                   /*isVarArg=*/false);
}

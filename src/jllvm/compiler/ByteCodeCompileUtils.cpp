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

llvm::GlobalVariable* jllvm::stringGlobal(llvm::Module& module, llvm::StringRef contents)
{
    return getOrInsertImportingGlobal(module, mangleStringGlobal(contents), /*addressSpace=*/1);
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

llvm::Value* jllvm::extendToStackType(llvm::IRBuilder<>& builder, FieldType type, llvm::Value* value)
{
    return match(
        type,
        [&](BaseType baseType)
        {
            switch (baseType.getValue())
            {
                case BaseType::Boolean:
                case BaseType::Byte:
                case BaseType::Short:
                {
                    return builder.CreateSExt(value, builder.getInt32Ty());
                }
                case BaseType::Char:
                {
                    return builder.CreateZExt(value, builder.getInt32Ty());
                }
                default: return value;
            }
        },
        [&](const auto&) { return value; });
}

namespace
{
void placeInJavaSection(llvm::Function* function)
{
    std::string sectionName = "java";
    if (llvm::Triple(LLVM_HOST_TRIPLE).isOSBinFormatMachO())
    {
        sectionName = "__TEXT," + sectionName;
        sectionName += ",regular,pure_instructions";
    }
    function->setSection(sectionName);
}

llvm::Constant* constantStruct(llvm::ArrayRef<llvm::Constant*> structFields)
{
    assert(!structFields.empty());
    llvm::SmallVector<llvm::Type*> body =
        llvm::to_vector(llvm::map_range(structFields, std::mem_fn(&llvm::Value::getType)));

    auto* structType = llvm::StructType::get(structFields.front()->getContext(), body);
    return llvm::ConstantStruct::get(structType, structFields);
}

using namespace jllvm;

llvm::Constant* createMethodMetadata(llvm::LLVMContext& context, JavaMethodMetadata::Kind kind,
                                     CallingConvention callingConvention)
{
    llvm::IntegerType* byteType = llvm::Type::getInt8Ty(context);
    return constantStruct({llvm::ConstantInt::get(byteType, static_cast<std::uint8_t>(kind)),
                           llvm::ConstantInt::get(byteType, static_cast<std::uint8_t>(callingConvention))});
}

} // namespace

void jllvm::addJavaJITMethodMetadata(llvm::Function* function, const Method* method,
                                     CallingConvention callingConvention)
{
    static_assert(alignof(JavaMethodMetadata) <= alignof(JavaMethodMetadata::JITData));

    std::size_t alignmentRequirement = alignof(JavaMethodMetadata::JITData);
    function->setAlignment(llvm::Align(alignmentRequirement));

    llvm::PointerType* pointerType = llvm::PointerType::get(function->getContext(), 0);
    llvm::Constant* jitData = constantStruct({/*method=*/methodGlobal(*function->getParent(), method),
                                              /*DenseMap=*/llvm::ConstantPointerNull::get(pointerType)});
    llvm::Constant* methodMetadata =
        createMethodMetadata(function->getContext(), JavaMethodMetadata::Kind::JIT, callingConvention);

    // Both the general Java method metadata and the JIT specific metadata are placed prior to the function in a packed
    // struct. A packed struct causes no padding to be inserted between the two structures nor at the end of the
    // structure.
    // This makes it possible to access the method metadata using 'functionPointer[-1]' and the jit data using
    // 'methodMetadata[-1]' after casting pointers to the appropriate types.
    auto* structType = llvm::StructType::get(function->getContext(), {jitData->getType(), methodMetadata->getType()},
                                             /*isPacked=*/true);
    placeInJavaSection(function);
    function->setPrefixData(constantStruct(llvm::ConstantStruct::get(structType, {jitData, methodMetadata})));
}

void jllvm::addJavaNativeMethodMetadata(llvm::Function* function, const Method* method)
{
    static_assert(alignof(JavaMethodMetadata) <= alignof(JavaMethodMetadata::NativeData));

    std::size_t alignmentRequirement = alignof(JavaMethodMetadata::NativeData);
    function->setAlignment(llvm::Align(alignmentRequirement));

    llvm::Constant* nativeData = constantStruct({/*method=*/methodGlobal(*function->getParent(), method)});
    // JNI always uses the JIT calling convention.
    llvm::Constant* methodMetadata =
        createMethodMetadata(function->getContext(), JavaMethodMetadata::Kind::Native, CallingConvention::JIT);

    auto* structType = llvm::StructType::get(function->getContext(), {nativeData->getType(), methodMetadata->getType()},
                                             /*isPacked=*/true);
    placeInJavaSection(function);
    function->setPrefixData(constantStruct(llvm::ConstantStruct::get(structType, {nativeData, methodMetadata})));
}

void jllvm::addJavaInterpreterMethodMetadata(llvm::Function* function, CallingConvention callingConvention)
{
    static_assert(alignof(JavaMethodMetadata) <= alignof(JavaMethodMetadata::InterpreterData));

    std::size_t alignmentRequirement = alignof(JavaMethodMetadata::NativeData);
    function->setAlignment(llvm::Align(alignmentRequirement));

    llvm::IntegerType* byteType = llvm::Type::getInt8Ty(function->getContext());
    // The linker sets interpreter data. Sufficient space has to be allocated nevertheless.
    llvm::Constant* interpreterData =
        llvm::ConstantAggregateZero::get(llvm::ArrayType::get(byteType, sizeof(JavaMethodMetadata::InterpreterData)));
    llvm::Constant* methodMetadata =
        createMethodMetadata(function->getContext(), JavaMethodMetadata::Kind::Interpreter, callingConvention);

    auto* structType =
        llvm::StructType::get(function->getContext(), {interpreterData->getType(), methodMetadata->getType()},
                              /*isPacked=*/true);
    placeInJavaSection(function);
    function->setPrefixData(constantStruct(llvm::ConstantStruct::get(structType, {interpreterData, methodMetadata})));
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

llvm::FunctionType* jllvm::osrMethodSignature(FieldType returnType, CallingConvention callingConvention,
                                              llvm::LLVMContext& context)
{
    llvm::Type* llvmReturnType;
    switch (callingConvention)
    {
        case CallingConvention::Interpreter: llvmReturnType = llvm::IntegerType::getInt64Ty(context); break;
        case CallingConvention::JIT: llvmReturnType = descriptorToType(returnType, context); break;
    }
    return llvm::FunctionType::get(llvmReturnType, {llvm::PointerType::get(context, 0)},
                                   /*isVarArg=*/false);
}

llvm::CallBase* jllvm::initializeClassObject(llvm::IRBuilder<>& builder, llvm::Value* classObject, bool addDeopt)
{
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::Module* module = function->getParent();

    auto* initializedGEP = builder.CreateGEP(builder.getInt8Ty(), classObject,
                                             builder.getInt32(jllvm::ClassObject::getInitializedOffset()));
    auto* initialized = builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), initializedGEP),
                                             builder.getInt8(uint8_t(jllvm::InitializationStatus::Uninitialized)));

    auto* classInitializer = llvm::BasicBlock::Create(builder.getContext(), "uninitialized", function);
    auto* continueBlock = llvm::BasicBlock::Create(builder.getContext(), "initialized", function);
    builder.CreateCondBr(initialized, continueBlock, classInitializer);

    builder.SetInsertPoint(classInitializer);

    llvm::SmallVector<llvm::OperandBundleDef> deopts{llvm::OperandBundleDef{"deopt", std::nullopt}};
    if (!addDeopt)
    {
        deopts.clear();
    }

    llvm::CallBase* initialize = builder.CreateCall(
        module->getOrInsertFunction("jllvm_initialize_class_object", builder.getVoidTy(), classObject->getType()),
        classObject, deopts);

    builder.CreateBr(continueBlock);

    builder.SetInsertPoint(continueBlock);

    return initialize;
}

void jllvm::emitReturn(llvm::IRBuilder<>& builder, llvm::Value* value, CallingConvention callingConvention)
{
    switch (callingConvention)
    {
        case CallingConvention::Interpreter:
        {
            if (!value)
            {
                // For void methods returning any kind of value would suffice as it is never read.
                // C++ callers do not expect a 'poison' or 'undef' value however (as clang uses 'noundef' and 'nopoison'
                // return attributes), so avoid using those.
                builder.CreateRet(builder.getInt64(0));
                return;
            }

            // Translate the value returned by the JIT calling convention to the 'uint64_t' expected by the interpreter.
            llvm::Function* function = builder.GetInsertBlock()->getParent();
            llvm::TypeSize typeSize = function->getParent()->getDataLayout().getTypeSizeInBits(value->getType());
            assert(!typeSize.isScalable() && "return type is never a scalable type");

            llvm::IntegerType* intNTy = builder.getIntNTy(typeSize.getFixedValue());
            value = builder.CreateBitOrPointerCast(value, intNTy);
            value = builder.CreateZExtOrTrunc(value, function->getReturnType());
            builder.CreateRet(value);
            break;
        }
        case CallingConvention::JIT:
            if (!value)
            {
                builder.CreateRetVoid();
                return;
            }
            builder.CreateRet(value);
            break;
    }
}

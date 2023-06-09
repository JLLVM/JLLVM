#include "ByteCodeCompileUtils.hpp"

#include <jllvm/support/Variant.hpp>

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

llvm::Type* jllvm::descriptorToType(const FieldType& type, llvm::LLVMContext& context)
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

llvm::FunctionType* jllvm::descriptorToType(const MethodType& type, bool isStatic, llvm::LLVMContext& context)
{
    auto args = llvm::to_vector(
        llvm::map_range(type.parameters, [&](const auto& elem) { return descriptorToType(elem, context); }));
    if (!isStatic)
    {
        args.insert(args.begin(), referenceType(context));
    }
    return llvm::FunctionType::get(descriptorToType(type.returnType, context), args, false);
}

#include "ByteCodeLayer.hpp"

#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "jvm"

#include "ByteCodeMaterializationUnit.hpp"

std::string jllvm::mangleMethod(llvm::StringRef className, llvm::StringRef methodName, llvm::StringRef descriptor)
{
    return (className + "." + methodName + ":" + descriptor).str();
}

std::string jllvm::mangleMethod(const MethodInfo& methodInfo, const ClassFile& classFile)
{
    llvm::StringRef className = classFile.getThisClass();
    llvm::StringRef methodName = methodInfo.getName(classFile);
    llvm::StringRef descriptor = methodInfo.getDescriptor(classFile);

    return mangleMethod(className, methodName, descriptor);
}

llvm::Error jllvm::ByteCodeLayer::add(llvm::orc::JITDylib& dylib, const MethodInfo* methodInfo,
                                      const ClassFile* classFile, const Method* method, const ClassObject* classObject)
{
    return dylib.define(
        std::make_unique<ByteCodeMaterializationUnit>(*this, methodInfo, classFile, method, classObject));
}

llvm::orc::MaterializationUnit::Interface jllvm::ByteCodeLayer::getSymbolsProvided(const MethodInfo* methodInfo,
                                                                                   const ClassFile* classFile)
{
    llvm::orc::SymbolFlagsMap result;
    auto name = mangleMethod(*methodInfo, *classFile);
    result[m_interner(name)] = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
    return llvm::orc::MaterializationUnit::Interface{std::move(result), nullptr};
}

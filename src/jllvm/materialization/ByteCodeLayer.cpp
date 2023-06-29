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
    return llvm::orc::MaterializationUnit::Interface(std::move(result), nullptr);
}

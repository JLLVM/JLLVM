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

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LazyReexports.h>

#include <jllvm/class/ClassFile.hpp>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Applies the JNI name mangling to create the corresponding C symbol name for the given 'methodName' inside of
/// 'className'. If 'methodDescriptor' is non empty, it must be a valid method descriptor whose parameter types are
/// then also encoded in the symbol name (to allow overloading).
std::string formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName, MethodType methodType);

std::string formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName);

std::string formJNIMethodName(const Method* method, bool withType);

/// Layer implementing all JIT functionality related to the Java Native Interface. It is also where any JNI symbols
/// must be registered to be called at runtime. Its implementation roughly boils down to creating compile stubs for any
/// native methods registered and then looking up and generating bridge code once the native method has actually
/// been called.
class JNIImplementationLayer : public ByteCodeLayer
{
    llvm::orc::JITDylib& m_jniImpls;
    llvm::orc::IRLayer& m_irLayer;
    llvm::DataLayout m_dataLayout;
    void* m_jniNativeFunctions;

public:
    JNIImplementationLayer(llvm::orc::ExecutionSession& session, llvm::orc::MangleAndInterner& mangler,
                           llvm::orc::IRLayer& irLayer, const llvm::DataLayout& dataLayout, void* jniNativeFunctions)
        : ByteCodeLayer(mangler),
          m_jniImpls(session.createBareJITDylib("<jni>")),
          m_irLayer(irLayer),
          m_dataLayout(dataLayout),
          m_jniNativeFunctions(jniNativeFunctions)
    {
    }

    /// Adds a new materialization unit to the JNI dylib which will be used to lookup any symbols when 'native' methods
    /// are called.
    void define(std::unique_ptr<llvm::orc::MaterializationUnit>&& materializationUnit)
    {
        llvm::cantFail(m_jniImpls.define(std::move(materializationUnit)));
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method* method) override;
};
} // namespace jllvm

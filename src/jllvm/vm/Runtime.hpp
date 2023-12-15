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
#include <llvm/ExecutionEngine/Orc/EPCIndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/Target/TargetMachine.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <memory>

#include "Executor.hpp"
#include "OSRState.hpp"

namespace jllvm
{
class ClassLoader;
class VirtualMachine;

/// Class consolidating and abstracting the execution of JVM methods, regardless of where they are actually being
/// executed.
class Runtime
{
    std::unique_ptr<llvm::orc::ExecutionSession> m_session;

    std::vector<Executor*> m_executors;

    /// Dylib containing all Java method symbols in "direct-method-call" mangling using the C calling convention.
    llvm::orc::JITDylib& m_jitCCStubs;
    /// Mapping of a Java method to the executor it is being executed by.
    llvm::DenseMap<const Method*, Executor*> m_executorState;

    /// Dylib containing all functions that may be produced by compilation of LLVM. This mainly contains C library
    /// symbols and instrumentation symbols such as for ASAN.
    llvm::orc::JITDylib& m_clib;
    /// Dylib containing references to class objects or method objects.
    llvm::orc::JITDylib& m_classAndMethodObjects;
    std::unique_ptr<llvm::orc::EPCIndirectionUtils> m_epciu;
    std::unique_ptr<llvm::TargetMachine> m_targetMachine;
    llvm::orc::LazyCallThroughManager& m_lazyCallThroughManager;

    std::unique_ptr<llvm::orc::IndirectStubsManager> m_jitCCStubsManager;

    llvm::DataLayout m_dataLayout;
    ClassLoader& m_classLoader;

    llvm::orc::MangleAndInterner m_interner;
    llvm::orc::ObjectLinkingLayer m_objectLayer;
    llvm::orc::IRCompileLayer m_compilerLayer;
    llvm::orc::IRTransformLayer m_optimizeLayer;

    llvm::DenseSet<std::uintptr_t> m_javaFrames;

    void optimize(llvm::Module& module);

    void prepare(ClassObject& classObject);

public:
    /// Creates a runtime instance from a virtual machine and a list of executors.
    /// The list of executors must be the full list of executors that are capable of executing some JVM methods.
    explicit Runtime(VirtualMachine& virtualMachine, llvm::ArrayRef<Executor*> executors);

    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Returns the ORC execution session.
    llvm::orc::ExecutionSession& getSession() const
    {
        return *m_session;
    }

    /// Returns the dylib containing the stubs leading to execution of Java methods in the JIT calling convention.
    llvm::orc::JITDylib& getJITCCDylib() const
    {
        return m_jitCCStubs;
    }

    /// Returns the dylib containing C library symbols and similar that might be referenced as a side-effect of
    /// compiling with LLVM. This dylib should generally be in the link order of all dylibs used for lookups.
    llvm::orc::JITDylib& getCLibDylib() const
    {
        return m_clib;
    }

    /// Returns the dylib containing global symbols for class and method objects.
    /// The latter use "method-global" mangling while the former uses "class-object-global" mangling.
    llvm::orc::JITDylib& getClassAndMethodObjectsDylib() const
    {
        return m_classAndMethodObjects;
    }

    /// Returns the LLVM IR Layer that should be used by any LLVM IR producing layer.
    llvm::orc::IRLayer& getLLVMIRLayer()
    {
        return m_optimizeLayer;
    }

    /// Returns the datalayout that should be used when compiling LLVM IR.
    const llvm::DataLayout& getDataLayout() const
    {
        return m_dataLayout;
    }

    /// Returns the interner for symbol resolution.
    llvm::orc::MangleAndInterner& getInterner()
    {
        return m_interner;
    }

    /// Returns a new indirect stubs manager for use by other classes.
    std::unique_ptr<llvm::orc::IndirectStubsManager> createIndirectStubsManager()
    {
        return m_epciu->createIndirectStubsManager();
    }

    /// Registers the methods of 'classObject' within all executors and prepares it for execution.
    /// 'defaultExecutor' is used during registering as the initial executor that should be used when executing the
    /// methods within 'classObject' if possible.
    void add(ClassObject* classObject, Executor& defaultExecutor);

    /// Returns a pointer in the JIT calling convention to the method with the given name.
    /// This pointer can also be called from C++ when cast to the correct signature.
    /// Returns nullptr if no such method exists.
    void* lookupJITCC(llvm::StringRef name)
    {
        llvm::Expected<llvm::JITEvaluatedSymbol> symbol = m_session->lookup({&m_jitCCStubs}, m_interner(name));
        if (!symbol)
        {
            llvm::consumeError(symbol.takeError());
            return nullptr;
        }
        return reinterpret_cast<void*>(symbol->getAddress());
    }

    void* lookupJITCC(llvm::StringRef className, llvm::StringRef methodName, MethodType descriptor)
    {
        return lookupJITCC(mangleDirectMethodCall(className, methodName, descriptor));
    }

    void* lookupJITCC(const Method& method)
    {
        return lookupJITCC(mangleDirectMethodCall(&method));
    }

    template <class Fn>
    Fn* lookupJITCC(llvm::StringRef name)
    {
        static_assert(std::is_function_v<Fn>);
        return reinterpret_cast<Fn*>(lookupJITCC(name));
    }

    template <class Fn>
    Fn* lookupJITCC(llvm::StringRef className, llvm::StringRef methodName, MethodType descriptor)
    {
        static_assert(std::is_function_v<Fn>);
        return reinterpret_cast<Fn*>(lookupJITCC(className, methodName, descriptor));
    }

    /// Returns the metadata associated with any Java method.
    /// Returns a null pointer if the function pointer is not a Java method.
    const JavaMethodMetadata* getJavaMethodMetadata(std::uintptr_t functionPointer) const
    {
        if (!m_javaFrames.contains(functionPointer))
        {
            return nullptr;
        }
        return &reinterpret_cast<const JavaMethodMetadata*>(functionPointer)[-1];
    }

    /// Performs On-Stack-Replacement of 'frame' and all its callees, replacing it with the execution of the same
    /// method. The abstract machine state of the new execution is initialized with 'state'.
    [[noreturn]] void doOnStackReplacement(JavaFrame frame, OSRState&& state);
};
} // namespace jllvm

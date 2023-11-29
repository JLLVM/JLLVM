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

#include <llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/EPCIndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ThreadPool.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/gc/GarbageCollector.hpp>
#include <jllvm/materialization/ByteCodeCompileLayer.hpp>
#include <jllvm/materialization/ByteCodeOSRCompileLayer.hpp>
#include <jllvm/materialization/InterpreterOSRLayer.hpp>
#include <jllvm/materialization/JIT2InterpreterLayer.hpp>
#include <jllvm/materialization/JNIImplementationLayer.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/object/StringInterner.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <memory>

#include "JavaFrame.hpp"
#include "OSRState.hpp"

namespace jllvm
{

/// Where code should be executed by default.
enum class ExecutionMode
{
    /// Executes code in the JIT whenever possible.
    JIT,
    /// Executes code in the interpreter whenever possible.
    Interpreter,
    /// Dynamically adjusts where code is being executed.
    Mixed
};

class JIT
{
    std::unique_ptr<llvm::orc::ExecutionSession> m_session;
    /// Dylib containing ALL Java methods of all class objects ever added to the JIT. These stubs can change through
    /// the lifetime of the VM to dispatch to compile callbacks, JIT compiled methods and potentially more in the
    /// future.
    /// These stubs are also the only public interface into the JIT.
    llvm::orc::JITDylib& m_externalStubs;
    /// Dylib containing only the actual JIT compiled Java methods.
    llvm::orc::JITDylib& m_javaJITSymbols;
    llvm::orc::JITDylib& m_jit2InterpreterSymbols;
    /// Dylib containing all additional symbols required by 'm_javaJITSymbols' to link correctly, but are not part of
    /// the public interface of the JIT.
    llvm::orc::JITDylib& m_implDetails;
    std::unique_ptr<llvm::orc::EPCIndirectionUtils> m_epciu;
    std::unique_ptr<llvm::TargetMachine> m_targetMachine;
    llvm::orc::LazyCallThroughManager& m_lazyCallThroughManager;
    /// Stub manager used through the JIT but most importantly for the stubs within 'm_externalStubs'. The manager can
    /// be used to redirect these stubs to different implementations of the methods.
    std::unique_ptr<llvm::orc::IndirectStubsManager> m_externalStubsManager;

    ClassLoader& m_classLoader;
    StringInterner& m_stringInterner;

    llvm::DataLayout m_dataLayout;
    ExecutionMode m_executionMode;

    llvm::orc::MangleAndInterner m_interner;
    llvm::orc::ObjectLinkingLayer m_objectLayer;
    llvm::orc::IRCompileLayer m_compilerLayer;
    llvm::orc::IRTransformLayer m_optimizeLayer;
    ByteCodeCompileLayer m_byteCodeCompileLayer;
    ByteCodeOSRCompileLayer m_byteCodeOSRCompileLayer;
    JNIImplementationLayer m_jniLayer;
    JIT2InterpreterLayer m_compiled2InterpreterLayer;
    InterpreterOSRLayer m_interpreterOSRLayer;

    llvm::DenseSet<std::uintptr_t> m_javaFrames;

    void optimize(llvm::Module& module);

    JIT(std::unique_ptr<llvm::orc::ExecutionSession>&& session, std::unique_ptr<llvm::orc::EPCIndirectionUtils>&& epciu,
        llvm::orc::JITTargetMachineBuilder&& builder, llvm::DataLayout&& layout, ClassLoader& classLoader,
        GarbageCollector& gc, StringInterner& stringInterner, void* jniFunctions, ExecutionMode executionMode);

public:
    static jllvm::JIT create(ClassLoader& classLoader, GarbageCollector& gc, StringInterner& stringInterner,
                             void* jniFunctions, ExecutionMode executionMode);

    ~JIT();

    JIT(const JIT&) = delete;
    JIT(JIT&&) = delete;
    JIT& operator=(const JIT&) = delete;
    JIT& operator=(JIT&&) = delete;

    /// Adds a new materialization unit to the JNI dylib which will be used to lookup any symbols when 'native' methods
    /// are called.
    void addJNISymbols(std::unique_ptr<llvm::orc::MaterializationUnit>&& materializationUnit)
    {
        m_jniLayer.define(std::move(materializationUnit));
    }

    /// Adds a new function object 'f' implementing the JNI function 'symbol'. This function object will then be called
    /// if any Java code calls the native method corresponding to the JNI mangled name passed in as 'symbol'.
    /// 'f' must be trivially copyable type.
    template <class F>
    void addJNISymbol(std::string symbol, const F& f)
    {
        m_jniLayer.define(
            createLambdaMaterializationUnit(std::move(symbol), m_optimizeLayer, f, m_dataLayout, m_interner));
    }

    /// Add all symbol-implementation pairs to the implementation library.
    /// The implementation library contains implementation of functions used by the materialization
    /// (bytecode compiler, JNI bridge, etc.).
    template <class Ss, class... Fs>
    void addImplementationSymbols(std::pair<Ss, Fs>&&... args)
    {
        (addImplementationSymbol(std::move(args.first), std::move(args.second)), ...);
    }

    /// Add callable 'f' as implementation for symbol 'symbol' to the implementation library.
    template <class F>
    void addImplementationSymbol(std::string symbol, const F& f)
        requires(!std::is_pointer_v<F> && !std::is_function_v<F>)
    {
        llvm::cantFail(m_implDetails.define(
            createLambdaMaterializationUnit(std::move(symbol), m_optimizeLayer, f, m_dataLayout, m_interner)));
    }

    /// Add function pointer 'f' as implementation for symbol 'symbol' to the implementation library.
    template <class Ret, class... Args>
    void addImplementationSymbol(llvm::StringRef symbol, Ret (*f)(Args...))
    {
        llvm::cantFail(m_implDetails.define(llvm::orc::absoluteSymbols(
            {{m_interner(symbol), llvm::JITEvaluatedSymbol::fromPointer(f, llvm::JITSymbolFlags::Exported
                                                                               | llvm::JITSymbolFlags::Callable)}})));
    }

    /// Add 'ptr' as implementation of global 'symbol' to the implementation library.
    template <class T>
    void addImplementationSymbol(llvm::StringRef symbol, T* ptr) requires(!std::is_function_v<T>)
    {
        llvm::cantFail(m_implDetails.define(
            llvm::orc::absoluteSymbols({{m_interner(symbol), llvm::JITEvaluatedSymbol::fromPointer(ptr)}})));
    }

    /// Adds and registers a class in the JIT. This has to be done prior to being able to lookup and execute
    /// any methods defined within the class file.
    void add(const ClassObject* classObject);

    /// Returns the interner used by the JIT.
    llvm::orc::MangleAndInterner& getInterner()
    {
        return m_interner;
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

    /// Performs On-Stack-Replacement of 'frame' and all its callees, replacing it with the execution of the same
    /// method. This method is meant to be used for executing exception handlers and therefore puts only 'exception'
    /// on the operand stack. 'frame' must be the execution of a Java method.
    [[noreturn]] void doExceptionOnStackReplacement(JavaFrame frame, std::uint16_t handlerOffset, Throwable* exception);

    /// Performs On-Stack-Replacement of 'frame' and all its callees, replacing it with the execution of the same
    /// method.
    [[noreturn]] void doI2JOnStackReplacement(InterpreterFrame frame);

    /// Looks up the method 'methodName' within the class 'className' with the type given by 'methodDescriptor'
    /// returning a pointer to the function if successful or an error otherwise.
    llvm::Expected<llvm::JITEvaluatedSymbol> lookup(FieldType classDescriptor, llvm::StringRef methodName,
                                                    MethodType methodDescriptor)
    {
        return m_session->lookup({&m_externalStubs},
                                 m_interner(mangleDirectMethodCall(classDescriptor, methodName, methodDescriptor)));
    }
};
} // namespace jllvm

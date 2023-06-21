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
#include <jllvm/gc/GarbageCollector.hpp>
#include <jllvm/materialization/ByteCodeCompileLayer.hpp>
#include <jllvm/materialization/ByteCodeOnDemandLayer.hpp>
#include <jllvm/materialization/JNIImplementationLayer.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/object/ClassLoader.hpp>

#include <memory>

#include "StringInterner.hpp"

namespace jllvm
{
class JIT
{
    std::unique_ptr<llvm::orc::ExecutionSession> m_session;
    llvm::orc::JITDylib& m_main;
    llvm::orc::JITDylib& m_implementation;
    std::unique_ptr<llvm::orc::EPCIndirectionUtils> m_epciu;
    std::unique_ptr<llvm::TargetMachine> m_targetMachine;
    std::unique_ptr<llvm::orc::JITCompileCallbackManager> m_callbackManager;

    llvm::DataLayout m_dataLayout;

    llvm::orc::MangleAndInterner m_interner;
    llvm::orc::ObjectLinkingLayer m_objectLayer;
    llvm::orc::IRCompileLayer m_compilerLayer;
    llvm::orc::IRTransformLayer m_optimizeLayer;
    ByteCodeCompileLayer m_byteCodeCompileLayer;
    ByteCodeOnDemandLayer m_byteCodeOnDemandLayer;
    JNIImplementationLayer m_jniLayer;

    llvm::DenseSet<void*> m_javaFrames;

    void optimize(llvm::Module& module);

    JIT(std::unique_ptr<llvm::orc::ExecutionSession>&& session, std::unique_ptr<llvm::orc::EPCIndirectionUtils>&& epciu,
        llvm::orc::JITTargetMachineBuilder&& builder, llvm::DataLayout&& layout, ClassLoader& classLoader,
        GarbageCollector& gc, StringInterner& stringInterner, void* jniFunctions);

public:
    static jllvm::JIT create(ClassLoader& classLoader, GarbageCollector& gc, StringInterner& stringInterner,
                             void* jniFunctions);

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
        llvm::cantFail(m_implementation.define(
            createLambdaMaterializationUnit(std::move(symbol), m_optimizeLayer, f, m_dataLayout, m_interner)));
    }

    /// Add function pointer 'f' as implementation for symbol 'symbol' to the implementation library.
    template <class Ret, class... Args>
    void addImplementationSymbol(llvm::StringRef symbol, Ret (*f)(Args...))
    {
        llvm::cantFail(m_implementation.define(llvm::orc::absoluteSymbols(
            {{m_interner(symbol), llvm::JITEvaluatedSymbol::fromPointer(f, llvm::JITSymbolFlags::Exported
                                                                               | llvm::JITSymbolFlags::Callable)}})));
    }

    /// Add 'ptr' as implementation of global 'symbol' to the implementation library.
    template <class T>
    void addImplementationSymbol(llvm::StringRef symbol, T* ptr) requires(!std::is_function_v<T>)
    {
        llvm::cantFail(m_implementation.define(
            llvm::orc::absoluteSymbols({{m_interner(symbol), llvm::JITEvaluatedSymbol::fromPointer(ptr)}})));
    }

    /// Adds and registers a class file in the JIT. This has to be done prior to being able to lookup and execute
    /// any methods defined within the class file.
    void add(const ClassFile* classFile, const ClassObject* classObject);

    /// Returns the interner used by the JIT.
    llvm::orc::MangleAndInterner& getInterner()
    {
        return m_interner;
    }

    /// Returns the set of all function pointers that are compiled Java methods.
    const llvm::DenseSet<void*>& getJavaFrames() const
    {
        return m_javaFrames;
    }

    /// Returns the metadata associated with any compiled Java method.
    /// Returns an empty optional if the function pointer is not a Java method.
    std::optional<JavaMethodMetadata> getJavaMethodMetadata(std::uintptr_t functionPointer)
    {
        if (!m_javaFrames.contains(reinterpret_cast<void*>(functionPointer)))
        {
            return std::nullopt;
        }
        return reinterpret_cast<const JavaMethodMetadata*>(functionPointer)[-1];
    }

    /// Looks up the method 'methodName' within the class 'className' with the type given by 'methodDescriptor'
    /// returning a pointer to the function if successful or an error otherwise.
    llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef className, llvm::StringRef methodName,
                                                    llvm::StringRef methodDescriptor)
    {
        return m_session->lookup({&m_main}, m_interner(mangleMethod(className, methodName, methodDescriptor)));
    }
};
} // namespace jllvm

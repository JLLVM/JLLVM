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
#include <jllvm/materialization/ByteCodeCompileLayer.hpp>
#include <jllvm/materialization/ByteCodeOnDemandLayer.hpp>
#include <jllvm/materialization/JNIImplementationLayer.hpp>
#include <jllvm/object/ClassLoader.hpp>

#include <memory>

#include "GarbageCollector.hpp"

namespace jllvm
{

class JIT
{
    std::unique_ptr<llvm::orc::ExecutionSession> m_session;
    llvm::orc::JITDylib& m_main;
    std::unique_ptr<llvm::orc::EPCIndirectionUtils> m_epciu;
    std::unique_ptr<llvm::TargetMachine> m_targetMachine;
    std::unique_ptr<llvm::orc::JITCompileCallbackManager> m_callbackManager;

    llvm::DataLayout m_dataLayout;
    ClassLoader& m_classLoader;

    llvm::orc::MangleAndInterner m_interner;
    llvm::orc::ObjectLinkingLayer m_objectLayer;
    llvm::orc::IRCompileLayer m_compilerLayer;
    llvm::orc::IRTransformLayer m_optimizeLayer;
    ByteCodeCompileLayer m_byteCodeCompileLayer;
    ByteCodeOnDemandLayer m_byteCodeOnDemandLayer;
    JNIImplementationLayer m_jniLayer;
    llvm::ThreadPool m_threadPool;


    GarbageCollector& m_gc;

    void optimize(llvm::Module& module);

    JIT(std::unique_ptr<llvm::orc::ExecutionSession>&& session, std::unique_ptr<llvm::orc::EPCIndirectionUtils>&& epciu,
        llvm::orc::JITTargetMachineBuilder&& builder, llvm::DataLayout&& layout, ClassLoader& classLoader,
        GarbageCollector& gc, void* jniFunctions);

public:
    static JIT create(ClassLoader& classLoader, GarbageCollector& gc, void* jniFunctions);

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

    /// Adds and registers a class file in the JIT. This has to be done prior to being able to lookup and execute
    /// any methods defined within the class file.
    void add(const ClassFile* classFile);

    /// Returns the interner used by the JIT.
    llvm::orc::MangleAndInterner& getInterner()
    {
        return m_interner;
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

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
std::string formJNIMethodName(llvm::StringRef className, llvm::StringRef methodName,
                              llvm::StringRef methodDescriptor = {});

/// Layer implementing all JIT functionality related to the Java Native Interface. It is also where any JNI symbols
/// must be registered to be called at runtime. Its implementation roughly boils down to creating compile stubs for any
/// native methods registered and then looking up and generating bridge code once the native method has actually
/// been called.
class JNIImplementationLayer : public ByteCodeLayer
{
    llvm::orc::JITDylib& m_jniImpls;
    llvm::orc::JITDylib& m_jniBridges;
    std::unique_ptr<llvm::orc::IndirectStubsManager> m_stubsManager;
    llvm::orc::JITCompileCallbackManager& m_callbackManager;
    llvm::orc::IRLayer& m_irLayer;
    llvm::DataLayout m_dataLayout;
    void* m_jniNativeFunctions;

public:
    JNIImplementationLayer(llvm::orc::ExecutionSession& session,
                           std::unique_ptr<llvm::orc::IndirectStubsManager> stubsManager,
                           llvm::orc::JITCompileCallbackManager& callbackManager, llvm::orc::MangleAndInterner& mangler,
                           llvm::orc::IRLayer& irLayer, const llvm::DataLayout& dataLayout, void* jniNativeFunctions)
        : ByteCodeLayer(mangler),
          m_jniImpls(session.createBareJITDylib("<jni>")),
          m_jniBridges(session.createBareJITDylib("<jniBridge>")),
          m_stubsManager(std::move(stubsManager)),
          m_callbackManager(callbackManager),
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

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
              const ClassFile* classFile) override;
};
} // namespace jllvm

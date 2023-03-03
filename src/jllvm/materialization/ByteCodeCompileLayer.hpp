#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>

#include <jllvm/object/ClassLoader.hpp>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Layer for compiling a JVM method to LLVM IR and handing it to an IR Layer for further compilation.
class ByteCodeCompileLayer : public ByteCodeLayer
{
    ClassLoader& m_classLoader;
    llvm::orc::JITDylib& m_mainDylib;
    std::unique_ptr<llvm::orc::IndirectStubsManager> m_stubsManager;
    llvm::orc::JITCompileCallbackManager& m_callbackManager;
    llvm::orc::IRLayer& m_baseLayer;
    llvm::orc::JITDylib& m_stubsImplDylib;
    llvm::DataLayout m_dataLayout;

public:
    ByteCodeCompileLayer(ClassLoader& classLoader, llvm::orc::JITDylib& mainDylib,
                         std::unique_ptr<llvm::orc::IndirectStubsManager>&& stubsManager,
                         llvm::orc::JITCompileCallbackManager& callbackManager, llvm::orc::IRLayer& baseLayer,
                         llvm::orc::MangleAndInterner& mangler, const llvm::DataLayout& dataLayout)
        : ByteCodeLayer(mangler),
          m_classLoader(classLoader),
          m_stubsManager(std::move(stubsManager)),
          m_callbackManager(callbackManager),
          m_baseLayer(baseLayer),
          m_stubsImplDylib(baseLayer.getExecutionSession().createBareJITDylib("<stubsImpl>")),
          m_dataLayout(dataLayout),
          m_mainDylib(mainDylib)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
              const ClassFile* classFile) override;
};
} // namespace jllvm

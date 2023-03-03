#pragma once

#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <memory>

#include "GarbageCollector.hpp"
#include "JIT.hpp"

namespace jllvm
{

class VirtualMachine
{
    using JNINativeInterfaceUPtr = std::unique_ptr<void, void (*)(void*)>;

    JNINativeInterfaceUPtr createJNIEnvironment();

    JNINativeInterfaceUPtr m_jniEnv = createJNIEnvironment();
    ClassLoader m_classLoader;
    GarbageCollector m_gc;
    JIT m_jit = JIT::create(m_classLoader, m_gc, m_jniEnv.get());

public:
    VirtualMachine(std::vector<std::string>&& classPath);

    llvm::orc::MangleAndInterner& getInterner()
    {
        return m_jit.getInterner();
    }

    /// Adds a new materialization unit to the JNI dylib which will be used to lookup any symbols when 'native' methods
    /// are called.
    void addJNISymbols(std::unique_ptr<llvm::orc::MaterializationUnit>&& materializationUnit)
    {
        m_jit.addJNISymbols(std::move(materializationUnit));
    }

    int executeMain(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args);
};

} // namespace jllvm

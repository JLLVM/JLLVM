#pragma once

#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <memory>
#include <random>

#include "GarbageCollector.hpp"
#include "JIT.hpp"
#include "StringInterner.hpp"

namespace jllvm
{
class VirtualMachine
{
    using JNINativeInterfaceUPtr = std::unique_ptr<void, void (*)(void*)>;

    JNINativeInterfaceUPtr createJNIEnvironment();

    JNINativeInterfaceUPtr m_jniEnv = createJNIEnvironment();
    ClassLoader m_classLoader;
    GarbageCollector m_gc;
    StringInterner m_stringInterner;
    GCRef<Throwable> m_activeException = static_cast<GCRef<Throwable>>(m_gc.allocateStatic());
    JIT m_jit = JIT::create(m_classLoader, m_gc, m_stringInterner, m_jniEnv.get(), m_activeException.ref());
    std::mt19937 m_pseudoGen;
    std::uniform_int_distribution<std::uint32_t> m_hashIntDistrib;

public:
    VirtualMachine(std::vector<std::string>&& classPath);

    /// Returns a new pseudo random hash code for a Java object.
    /// Since we have a relocating garbage collector we use a similar strategy to V8, where we generate pseudo random
    /// uniformly distributed integers for each object exactly once and then store and reuse that as hash code
    /// throughout the program.
    /// Note: The value returned is non-deterministic between program executions and seeded at VM startup.
    ///       It also never returns 0, but may return any other value that fits within 'int32_t'.
    std::int32_t createNewHashCode();

    /// Returns the jit instance of the virtual machine.
    JIT& getJIT()
    {
        return m_jit;
    }

    int executeMain(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args);
};

} // namespace jllvm

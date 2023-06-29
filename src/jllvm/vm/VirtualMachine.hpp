#pragma once

#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/gc/GarbageCollector.hpp>
#include <jllvm/object/ClassObject.hpp>

#include <memory>
#include <random>

#include "JIT.hpp"
#include "StringInterner.hpp"

namespace jllvm
{

/// Options used to boot the VM.
struct BootOptions
{
    llvm::StringRef javaHome;
    std::vector<std::string> classPath;
    bool systemInitialization = true;
};

class VirtualMachine
{
    using JNINativeInterfaceUPtr = std::unique_ptr<void, void (*)(void*)>;

    JNINativeInterfaceUPtr createJNIEnvironment();

    JNINativeInterfaceUPtr m_jniEnv = createJNIEnvironment();
    ClassLoader m_classLoader;
    GarbageCollector m_gc;
    StringInterner m_stringInterner;
    GCRootRef<Throwable> m_activeException = static_cast<GCRootRef<Throwable>>(m_gc.allocateStatic());
    JIT m_jit = JIT::create(m_classLoader, m_gc, m_stringInterner, m_jniEnv.get());
    std::mt19937 m_pseudoGen;
    std::uniform_int_distribution<std::uint32_t> m_hashIntDistrib;
    GCRootRef<Thread> m_mainThread = static_cast<GCRootRef<Thread>>(m_gc.allocateStatic());
    GCRootRef<Object> m_mainThreadGroup = m_gc.allocateStatic();
    std::string m_javaHome;

    void initialize(ClassObject& classObject);

public:
    explicit VirtualMachine(BootOptions&& options);

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

    /// Returns the garbage collector instance of the virtual machine.
    GarbageCollector& getGC()
    {
        return m_gc;
    }

    /// Returns the class loader instance of the virtual machine.
    ClassLoader& getClassLoader()
    {
        return m_classLoader;
    }

    /// Returns the main thread this VM is started on.
    GCRootRef<Thread> getMainThread() const
    {
        return m_mainThread;
    }

    /// Returns the string interner instance of the virtual machine.
    StringInterner& getStringInterner()
    {
        return m_stringInterner;
    }

    /// Returns Java home of this VM, or in other words, its installation directory (excluding the 'bin' directory)
    llvm::StringRef getJavaHome() const
    {
        return m_javaHome;
    }

    int executeMain(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args);

    template <class... Args>
    void executeObjectConstructor(GCRootRef<ObjectInterface> object, llvm::StringRef methodDescriptor, Args... args)
    {
        auto addr = llvm::cantFail(m_jit.lookup(object->getClass()->getClassName(), "<init>", methodDescriptor));
        reinterpret_cast<void (*)(ObjectInterface*, Args...)>(addr.getAddress())(object.address(), args...);
    }

    /// Calls the static method 'methodName' with types 'methodDescriptor' within 'className' using 'args'.
    template <class Ret = void, class... Args>
    Ret executeStaticMethod(llvm::StringRef className, llvm::StringRef methodName, llvm::StringRef methodDescriptor,
                            Args... args)
    {
        auto addr = llvm::cantFail(m_jit.lookup(className, methodName, methodDescriptor));
        return reinterpret_cast<Ret (*)(Args...)>(addr.getAddress())(args...);
    }
};

template <class T>
struct CppToLLVMType<jllvm::GCRootRef<T>> : CppToLLVMType<void*>
{
};

} // namespace jllvm

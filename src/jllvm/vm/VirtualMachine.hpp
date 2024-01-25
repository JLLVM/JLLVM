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

#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/gc/GarbageCollector.hpp>
#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/object/StringInterner.hpp>

#include <memory>
#include <random>

#include "Interpreter.hpp"
#include "JIT.hpp"
#include "JNIBridge.hpp"
#include "JavaFrame.hpp"
#include "Runtime.hpp"

struct JNINativeInterface_;

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

/// Options used to boot the VM.
struct BootOptions
{
    std::string javaHome;
    std::vector<std::string> classPath;
    bool systemInitialization = true;
    ExecutionMode executionMode = ExecutionMode::Mixed;
    std::string debugLogging;
};

struct ModelState;

class VirtualMachine
{
    using JNINativeInterfaceUPtr = std::unique_ptr<JNINativeInterface_, void (*)(JNINativeInterface_*)>;

    JNINativeInterfaceUPtr createJNIEnvironment();

    JNINativeInterfaceUPtr m_jniEnv = createJNIEnvironment();
    StringInterner m_stringInterner;
    ClassLoader m_classLoader;
    GarbageCollector m_gc;
    Runtime m_runtime;
    JIT m_jit;
    Interpreter m_interpreter;
    JNIBridge m_jni;
    std::mt19937 m_pseudoGen;
    std::uniform_int_distribution<std::uint32_t> m_hashIntDistrib;
    GCRootRef<Object> m_mainThread = m_gc.allocateStatic();
    GCRootRef<Object> m_mainThreadGroup = m_gc.allocateStatic();
    std::string m_javaHome;
    ExecutionMode m_executionMode;

    // Instances of 'Model::State', subtypes of ModelState.
    std::vector<std::unique_ptr<ModelState>> m_modelState;

    /// Returns the executor that should be used by default when first executing a method.
    Executor& getDefaultExecutor()
    {
        return getDefaultOSRTarget();
    }

    /// Returns the OSR target that should be used by default when performing OSR.
    OSRTarget& getDefaultOSRTarget()
    {
        OSRTarget* defaultExecutor = &m_interpreter;
        if (m_executionMode == ExecutionMode::JIT)
        {
            defaultExecutor = &m_jit;
        }
        return *defaultExecutor;
    }

    explicit VirtualMachine(BootOptions&& options);

public:

    /// Creates and boots a new instance of a 'VirtualMachine'.
    static VirtualMachine create(BootOptions&& options);

    VirtualMachine(const VirtualMachine&) = delete;

    VirtualMachine(VirtualMachine&&) = delete;

    VirtualMachine& operator=(const VirtualMachine&) = delete;

    VirtualMachine& operator=(VirtualMachine&&) = delete;

    ~VirtualMachine();

    /// Returns a new pseudo random hash code for a Java object.
    /// Since we have a relocating garbage collector we use a similar strategy to V8, where we generate pseudo random
    /// uniformly distributed integers for each object exactly once and then store and reuse that as hash code
    /// throughout the program.
    /// Note: The value returned is non-deterministic between program executions and seeded at VM startup.
    ///       It also never returns 0, but may return any other value that fits within 'int32_t'.
    std::int32_t createNewHashCode();

    /// Returns the runtime instance of this VM.
    Runtime& getRuntime()
    {
        return m_runtime;
    }

    /// Returns the JNI Bridge of this VM.
    JNIBridge& getJNIBridge()
    {
        return m_jni;
    }

    /// Returns the JNI Native Interface table of this VM.
    JNINativeInterface_* getJNINativeInterface() const
    {
        return m_jniEnv.get();
    }

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
    GCRootRef<Object> getMainThread() const
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

    /// Calls the constructor of 'object' with the types described by 'methodDescriptor' using 'args'.
    template <JavaConvertible... Args>
    void executeObjectConstructor(ObjectInterface* object, MethodType methodDescriptor, Args... args)
    {
        const Method* method = object->getClass()->getMethod("<init>", methodDescriptor);
        assert(method);
        method->call(object, args...);
    }

    /// Calls the static method 'methodName' with types 'methodDescriptor' within 'className' using 'args'.
    template <JavaCompatible Ret = void, JavaConvertible... Args>
    Ret executeStaticMethod(llvm::StringRef className, llvm::StringRef methodName, MethodType methodDescriptor,
                            Args... args)
    {
        void* addr = m_runtime.lookupJITCC(className, methodName, methodDescriptor);
        assert(addr);
        return invokeJava<Ret>(addr, args...);
    }

    /// Performs class initialization for 'classObject'. This is a noop if 'classObject' is not uninitialized.
    void initialize(ClassObject& classObject);

    /// Throws a Java exception which can be caught by exception handlers in Java. This also causes stack unwinding in
    /// C++ code, executing destructors as a C++ exception would.
    /// If no Java exception handler exists, 'exception' will be thrown as a C++ exception which can be caught as a
    /// 'const Throwable&'. It is otherwise not possible to catch 'exception' from C++ code if a exception handler was
    /// found in Java code.
    [[noreturn]] void throwJavaException(Throwable* exception);

    /// Constructs and throws a Java exception which can be caught by exception handlers in Java as detailed above.
    template <JavaConvertible... Args>
    [[noreturn]] void throwException(FieldType exceptionType, MethodType constructor, Args... args)
    {
        GCUniqueRoot exception = m_gc.root(m_gc.allocate<Throwable>(&m_classLoader.forName(exceptionType)));
        executeObjectConstructor(exception, constructor, args...);
        throwJavaException(exception);
    }

    /// Construct and throws an 'ArrayIndexOutOfBoundsException' with a message created from the index that was accessed
    /// and the length of the array.
    [[noreturn]] void throwArrayIndexOutOfBoundsException(std::int32_t indexAccessed, std::int32_t arrayLength);

    /// Construct and throws an 'ClassCastException' with a message created from the object being cast and the class
    /// object of the type that was attempted to be cast to.
    [[noreturn]] void throwClassCastException(ObjectInterface* object, ClassObject* classObject);

    /// Construct and throws a 'NegativeArraySizeException' with a message created from the length that the array was
    /// attempted to be constructed with.
    [[noreturn]] void throwNegativeArraySizeException(std::int32_t arrayLength);

    /// Construct and throws a 'NullPointerException' with the default constructor.
    [[noreturn]] void throwNullPointerException();

    /// Performs stack unwinding, calling 'f' for every Java frame encountered.
    /// 'f' may optionally return a 'UnwindAction' to control whether unwinding should continue.
    /// Returns true if 'UnwindAction::UnwindAction' was ever returned.
    template <std::invocable<JavaFrame> F>
    bool unwindJavaStack(F&& f)
    {
        return unwindStack(
            [&, this](UnwindFrame& frame)
            {
                const JavaMethodMetadata* metadata = m_runtime.getJavaMethodMetadata(frame.getFunctionPointer());
                if (!metadata)
                {
                    return UnwindAction::ContinueUnwinding;
                }

                using T = decltype(f(std::declval<JavaFrame>()));
                if constexpr (std::is_void_v<T>)
                {
                    f(JavaFrame(*metadata, frame));
                    return UnwindAction::ContinueUnwinding;
                }
                else
                {
                    return f(JavaFrame(*metadata, frame));
                }
            });
    }

    /// Default constructs a 'Model::State' instance within the VM and returns it.
    /// The lifetime of this object is equal to the lifetime of the VM.
    template <class State>
    State& allocModelState()
    {
        return static_cast<State&>(*m_modelState.emplace_back(std::make_unique<State>()));
    }
};

template <class T>
struct CppToLLVMType<jllvm::GCRootRef<T>> : CppToLLVMType<void*>
{
};

} // namespace jllvm

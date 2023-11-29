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
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/object/StringInterner.hpp>

#include <memory>
#include <random>

#include "InteropHelpers.hpp"
#include "Interpreter.hpp"
#include "JIT.hpp"
#include "JavaFrame.hpp"

namespace jllvm
{

/// Options used to boot the VM.
struct BootOptions
{
    llvm::StringRef javaHome;
    std::vector<std::string> classPath;
    bool systemInitialization = true;
    ExecutionMode executionMode = ExecutionMode::Mixed;
};

struct ModelState;

class VirtualMachine
{
    using JNINativeInterfaceUPtr = std::unique_ptr<void, void (*)(void*)>;

    JNINativeInterfaceUPtr createJNIEnvironment();

    JNINativeInterfaceUPtr m_jniEnv = createJNIEnvironment();
    ClassLoader m_classLoader;
    GarbageCollector m_gc;
    StringInterner m_stringInterner;
    JIT m_jit;
    Interpreter m_interpreter;
    std::mt19937 m_pseudoGen;
    std::uniform_int_distribution<std::uint32_t> m_hashIntDistrib;
    GCRootRef<Object> m_mainThread = m_gc.allocateStatic();
    GCRootRef<Object> m_mainThreadGroup = m_gc.allocateStatic();
    std::string m_javaHome;

    // Instances of 'Model::State', subtypes of ModelState.
    std::vector<std::unique_ptr<ModelState>> m_modelState;

public:
    explicit VirtualMachine(BootOptions&& options);

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
        auto addr = llvm::cantFail(m_jit.lookup(object->getClass()->getDescriptor(), "<init>", methodDescriptor));
        invokeJava<void>(addr, object, args...);
    }

    /// Calls the static method 'methodName' with types 'methodDescriptor' within 'className' using 'args'.
    template <JavaCompatible Ret = void, JavaConvertible... Args>
    Ret executeStaticMethod(FieldType classDescriptor, llvm::StringRef methodName, MethodType methodDescriptor,
                            Args... args)
    {
        auto addr = llvm::cantFail(m_jit.lookup(classDescriptor, methodName, methodDescriptor));
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

    /// Performs stack unwinding, calling 'f' for every Java frame encountered.
    /// 'f' may optionally return a 'UnwindAction' to control whether unwinding should continue.
    /// Returns true if 'UnwindAction::UnwindAction' was ever returned.
    template <std::invocable<JavaFrame> F>
    bool unwindJavaStack(F&& f)
    {
        return unwindStack(
            [&, this](UnwindFrame& frame)
            {
                const JavaMethodMetadata* metadata = m_jit.getJavaMethodMetadata(frame.getFunctionPointer());
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

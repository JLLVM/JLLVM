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
#include "JIT.hpp"

namespace jllvm
{

/// Default 'State' type of a 'ModelBase' subclass.
struct ModelState
{
    ModelState() = default;

    ModelState(const ModelState&) = delete;

    ModelState(ModelState&&) = delete;

    ModelState& operator=(const ModelState&) = delete;

    ModelState& operator=(ModelState&&) = delete;

    virtual ~ModelState() = default;
};

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
    GCRootRef<Object> m_mainThread = m_gc.allocateStatic();
    GCRootRef<Object> m_mainThreadGroup = m_gc.allocateStatic();
    std::string m_javaHome;

    void initialize(ClassObject& classObject);

    // Type erased instances of 'Model::State'.
    // The deleter casts the 'void*' back to its real type for destruction.
    std::vector<std::unique_ptr<ModelState>> m_modelState;

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
        auto addr = llvm::cantFail(m_jit.lookup(object->getClass()->getClassName(), "<init>", methodDescriptor));
        invokeJava<void>(addr, object, args...);
    }

    /// Calls the static method 'methodName' with types 'methodDescriptor' within 'className' using 'args'.
    template <JavaCompatible Ret = void, JavaConvertible... Args>
    Ret executeStaticMethod(llvm::StringRef className, llvm::StringRef methodName, MethodType methodDescriptor,
                            Args... args)
    {
        auto addr = llvm::cantFail(m_jit.lookup(className, methodName, methodDescriptor));
        return invokeJava<Ret>(addr, args...);
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

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

#include "VirtualMachine.hpp"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/TargetSelect.h>

#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include "NativeImplementation.hpp"

#define DEBUG_TYPE "jvm"

jllvm::VirtualMachine::VirtualMachine(BootOptions&& bootOptions)
    : m_classLoader(
          m_stringInterner, std::move(bootOptions.classPath), [this, bootOptions](ClassObject& classObject)
          { m_runtime.add(&classObject, getDefaultExecutor()); }, [&] { return m_gc.allocateStatic(); }),
      m_runtime(*this, {&m_jit, &m_interpreter, &m_jni}),
      m_jit(*this),
      m_interpreter(*this, /*backEdgeThreshold=*/bootOptions.backEdgeThreshold,
                    /*invocationThreshold=*/bootOptions.invocationThreshold),
      m_jni(*this, m_jniEnv.get()),
      m_gc(/*random value for now*/ 1 << 20),
      // Seed from the C++ implementations entropy source.
      m_pseudoGen(std::random_device{}()),
      // Exclude 0 from the output as that is our sentinel value for "not yet calculated".
      m_hashIntDistrib(1, std::numeric_limits<std::uint32_t>::max()),
      m_javaHome(bootOptions.javaHome),
      m_executionMode(bootOptions.executionMode)
{
    registerJavaClasses(*this);

    m_gc.addRootObjectsProvider(
        [&](GarbageCollector::RootProvider::AddRootObjectFn addRootObjectFn)
        {
            for (ClassObject* classObject : m_classLoader.getLoadedClassObjects())
            {
                addRootObjectFn(classObject);
            }
        });
    m_gc.addRootsForRelocationProvider(
        [this](GarbageCollector::RootProvider::RelocateObjectFn relocateObjectFn)
        {
            unwindJavaStack(
                [=](JavaFrame javaFrame)
                {
                    std::optional interpreterFrame = llvm::dyn_cast<InterpreterFrame>(javaFrame);
                    if (!interpreterFrame)
                    {
                        return;
                    }

                    auto addRoots = [=](llvm::MutableArrayRef<std::uint64_t> array, BitArrayRef<> mask)
                    {
                        for (auto&& [iter, isReference] : llvm::zip_equal(array, mask))
                        {
                            if (!isReference)
                            {
                                continue;
                            }

                            // Create a local variable of type 'ObjectInterface*' to be able to pass a refernece to it.
                            // 'reinterpret_cast<ObjectInterface**>(&iter)' would break C++ strict aliasing rules.
                            auto* object = llvm::bit_cast<ObjectInterface*>(static_cast<std::uintptr_t>(iter));
                            relocateObjectFn(object);
                            // Write back the update in case '*object' was relocated.
                            iter = llvm::bit_cast<std::uintptr_t>(object);
                        }
                    };

                    addRoots(interpreterFrame->getLocals(), interpreterFrame->getLocalsGCMask());
                    addRoots(interpreterFrame->getOperandStack(), interpreterFrame->getOperandStackGCMask());
                });
        });

    initialize(m_classLoader.loadBootstrapClasses());

    m_stringInterner.initialize(
        [&](FieldType descriptor)
        {
            ClassObject* classObject = &m_classLoader.forName(descriptor);
            initialize(*classObject);
            return classObject;
        });

    if (!bootOptions.systemInitialization)
    {
        return;
    }

    ClassObject& threadGroup = m_classLoader.forName("Ljava/lang/ThreadGroup;");
    initialize(threadGroup);
    m_mainThreadGroup.assign(m_gc.allocate(&threadGroup));
    executeObjectConstructor(m_mainThreadGroup, "()V");

    ClassObject& thread = m_classLoader.forName("Ljava/lang/Thread;");
    initialize(thread);
    m_mainThread.assign(m_gc.allocate(&thread));

    // These have to be set prior to the constructor for the constructor not to fail.
    thread.getInstanceField<std::int32_t>("priority", "I")(m_mainThread) = 1;
    thread.getInstanceField<std::int32_t>("threadStatus", "I")(m_mainThread) =
        static_cast<std::int32_t>(ThreadState::Runnable);

    executeObjectConstructor(m_mainThread, "(Ljava/lang/ThreadGroup;Ljava/lang/String;)V", m_mainThreadGroup,
                             m_stringInterner.intern("main"));

    initialize(m_classLoader.forName("Ljava/lang/System;"));
    executeStaticMethod("java/lang/System", "initPhase1", "()V");
}

jllvm::VirtualMachine::~VirtualMachine() = default;

int jllvm::VirtualMachine::executeMain(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args)
{
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer)
    {
        llvm::report_fatal_error("Failed to open " + path);
    }

    ClassObject& classObject = m_classLoader.add(std::move(*buffer));
    initialize(classObject);

    const Method* method = classObject.getMethod("main", "([Ljava/lang/String;)V");
    if (!method || method->isAbstract())
    {
        llvm::report_fatal_error("Failed to find main method in " + classObject.getClassName());
    }

    auto* javaArgs = m_gc.allocate<Array<String*>>(&m_classLoader.forName("[Ljava/lang/String;"), args.size());

    llvm::transform(args, javaArgs->begin(), [&](llvm::StringRef arg) { return m_stringInterner.intern(arg); });

    try
    {
        method->call(javaArgs);
        return 0;
    }
    catch (const Throwable& activeException)
    {
        // TODO: Use printStackTrace:()V in the future

        // Equivalent to Throwable:toString() (does not yet work for all Throwables).
        llvm::errs() << activeException.getClass()->getDescriptor().pretty();
        if (activeException.detailMessage)
        {
            llvm::errs() << ": " << activeException.detailMessage->toUTF8();
        }
        llvm::errs() << '\n';

        return -1;
    }
}

std::int32_t jllvm::VirtualMachine::createNewHashCode()
{
    return m_hashIntDistrib(m_pseudoGen);
}

void jllvm::VirtualMachine::initialize(ClassObject& classObject)
{
    if (!classObject.isUnintialized())
    {
        return;
    }

    classObject.setInitializationStatus(InitializationStatus::UnderInitialization);

    // 5.5 Step 7:
    // Next, if C is a class rather than an interface, then let SC be its superclass and let SI1, ..., SIn be
    // all superinterfaces of C (whether direct or indirect) that declare at least one non-abstract, non-static
    // method. The order of superinterfaces is given by a recursive enumeration over the superinterface
    // hierarchy of each interface directly implemented by C. For each interface I directly implemented by C (in
    // the order of the interfaces array of C), the enumeration recurs on I's superinterfaces (in the order of
    // the interfaces array of I) before returning I.
    //
    // For each S in the list [ SC, SI1, ..., SIn ], if S has not yet been initialized, then recursively perform
    // this entire procedure for S. If necessary, verify and prepare S first.
    // TODO: Implement above in detail.
    for (ClassObject* base : classObject.getBases())
    {
        initialize(*base);
    }

    auto* classInitializer = classObject.getMethod("<clinit>", "()V");
    if (!classInitializer)
    {
        return;
    }

    LLVM_DEBUG({
        llvm::dbgs() << "Executing class initializer "
                     << mangleDirectMethodCall(classObject.getClassName(), "<clinit>", "()V") << '\n';
    });
    classInitializer->call();
    classObject.setInitializationStatus(InitializationStatus::Initialized);
}

void jllvm::VirtualMachine::throwJavaException(Throwable* exception)
{
    unwindJavaStack(
        [&](JavaFrame frame)
        {
            std::optional<std::uint16_t> byteCodeOffset = frame.getByteCodeOffset();
            if (!byteCodeOffset)
            {
                return;
            }

            Code* code = frame.getMethod()->getMethodInfo().getAttributes().find<Code>();
            assert(code && "cannot be in a Java frame of a method without code");

            const ClassFile& classFile = *frame.getClassObject()->getClassFile();
            // Exception handler to use is the very first one in the [start, end) range where the type of the exception
            // is an instance of the catch type.
            std::optional<std::uint16_t> handlerPc;
            for (const Code::ExceptionTable* exceptionTable : code->getHandlersAt(*byteCodeOffset))
            {
                // Catch-all handlers as is used by 'finally' blocks don't have a catch type.
                if (!exceptionTable->catchType)
                {
                    handlerPc = exceptionTable->handlerPc;
                    break;
                }

                const ClassInfo* info = exceptionTable->catchType.resolve(classFile);
                ClassObject* catchType =
                    m_classLoader.forNameLoaded(ObjectType(info->nameIndex.resolve(classFile)->text));
                if (!catchType)
                {
                    // If the type to catch is not loaded, then it's impossible for the exception to be an instance.
                    continue;
                }
                if (exception->instanceOf(catchType))
                {
                    // Found the correct exception handler.
                    handlerPc = exceptionTable->handlerPc;
                    break;
                }
            }

            if (!handlerPc)
            {
                return;
            }

            m_runtime.doOnStackReplacement(
                frame, getDefaultOSRTarget().createOSRStateForExceptionHandler(frame, *handlerPc, exception));
        });

    // If no Java frame is ready to handle the exception, unwind all of it completely.
    // The caller of Javas main or the start of a Java thread will catch this in C++ code.
    throw *exception;
}

void jllvm::VirtualMachine::throwArrayIndexOutOfBoundsException(std::int32_t indexAccessed, std::int32_t arrayLength)
{
    String* string = m_stringInterner.intern(
        llvm::formatv("Index {0} out of bounds for length {1}", indexAccessed, arrayLength).str());
    throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
}

void jllvm::VirtualMachine::throwClassCastException(ObjectInterface* object, ClassObject* classObject)
{
    std::string className = object->getClass()->getDescriptor().pretty();
    std::string name = classObject->getDescriptor().pretty();
    llvm::StringRef prefix = classObject->isClass() || classObject->isInterface() ? "class " : "";

    String* string =
        m_stringInterner.intern(llvm::formatv("class {0} cannot be cast to {1}{2}", className, prefix, name).str());
    throwException("Ljava/lang/ClassCastException;", "(Ljava/lang/String;)V", string);
}

void jllvm::VirtualMachine::throwNegativeArraySizeException(std::int32_t arrayLength)
{
    String* string = m_stringInterner.intern(std::to_string(arrayLength));
    throwException("Ljava/lang/NegativeArraySizeException;", "(Ljava/lang/String;)V", string);
}

void jllvm::VirtualMachine::throwNullPointerException()
{
    throwException("Ljava/lang/NullPointerException;", "()V");
}

jllvm::VirtualMachine jllvm::VirtualMachine::create(BootOptions&& options)
{
    // Setup the global state in LLVM as is required by our VM.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::cl::ResetAllOptionOccurrences();

    llvm::SmallVector<const char*> llvmArgs{"jllvm"};

    // Deopt values are read-only and can be read from CSR registers by libunwind.
    llvmArgs.push_back("-use-registers-for-deopt-values=1");

#ifndef NDEBUG
    std::string temp = "-debug-only=" + options.debugLogging;
    llvmArgs.push_back("-jllvm-gc-every-alloc=1");
    if (!options.debugLogging.empty())
    {
        llvmArgs.push_back(temp.c_str());
    }
#endif
    llvm::cl::ParseCommandLineOptions(llvmArgs.size(), llvmArgs.data());

    return VirtualMachine(std::move(options));
}

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

#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include "NativeImplementation.hpp"

#define DEBUG_TYPE "jvm"

namespace
{
bool canOverride(const jllvm::Method& derived, const jllvm::ClassObject* derivedClass, const jllvm::Method& base,
                 const jllvm::ClassObject* baseClass)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.5
    if (derived.getName() != base.getName() || derived.getType() != base.getType())
    {
        return false;
    }

    switch (base.getVisibility())
    {
        case jllvm::Visibility::Private: return false;
        case jllvm::Visibility::Public:
        case jllvm::Visibility::Protected: return true;
        case jllvm::Visibility::Package:
            // 5.4.5 mA is marked neither ACC_PUBLIC nor ACC_PROTECTED nor ACC_PRIVATE, and either (a) the declaration
            // of mA appears in the same run-time package as the declaration of mC.
            // TODO: I am pretty sure this is not how the spec defines packages, but it'll do for now.
            if (derivedClass->getPackageName() == baseClass->getPackageName())
            {
                return true;
            }

            // TODO: 5.4.5 b)
            llvm_unreachable("NOT YET IMPLEMENTED");
    }
    llvm_unreachable("All visibilities handled");
}

jllvm::VTableSlot methodSelection(jllvm::JIT& jit, const jllvm::ClassObject* classObject,
                                  const jllvm::Method& resolvedMethod, const jllvm::ClassObject* resolvedMethodClass)
{
    auto doLookup = [&](llvm::StringRef className) -> jllvm::VTableSlot
    {
        return reinterpret_cast<jllvm::VTableSlot>(
            llvm::cantFail(jit.lookup(className, resolvedMethod.getName(), resolvedMethod.getType())).getAddress());
    };

    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.6 Step 1

    if (resolvedMethod.getVisibility() == jllvm::Visibility::Private)
    {
        return doLookup(resolvedMethodClass->getClassName());
    }

    // Step 2

    // If C contains a declaration of an instance method m that can override mR (§5.4.5), then m is the selected method.

    // Otherwise, if C has a superclass, a search for a declaration of an instance method that can override mR is
    // performed, starting with the direct superclass of C and continuing with the direct superclass of that class, and
    // so forth, until a method is found or no further superclasses exist. If a method is found, it is the selected
    // method.
    for (const jllvm::ClassObject* curr : classObject->getSuperClasses())
    {
        const auto* result = llvm::find_if(
            curr->getMethods(), [&](const jllvm::Method& method)
            { return !method.isStatic() && canOverride(method, curr, resolvedMethod, resolvedMethodClass); });
        if (result == curr->getMethods().end())
        {
            continue;
        }
        if (result->isAbstract())
        {
            return nullptr;
        }
        return doLookup(curr->getClassName());
    }

    // Otherwise, the maximally-specific superinterface methods of C are determined (§5.4.3.3). If exactly one matches
    // mR's name and descriptor and is not abstract, then it is the selected method.

    // A maximally-specific superinterface method of a class or interface C for a particular method name and descriptor
    // is any method for which all of the following are true:
    //
    // The method is declared in a superinterface (direct or indirect) of C.
    //
    // The method is declared with the specified name and descriptor.
    //
    // The method has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set.
    //
    // Where the method is declared in interface I, there exists no other maximally-specific superinterface method of C
    // with the specified name and descriptor that is declared in a subinterface of I.

    for (const jllvm::ClassObject* interface : classObject->maximallySpecificInterfaces())
    {
        if (llvm::any_of(interface->getMethods(),
                         [&](const jllvm::Method& method)
                         {
                             return !method.isStatic() && method.getVisibility() != jllvm::Visibility::Private
                                    && !method.isAbstract()
                                    && canOverride(method, interface, resolvedMethod, resolvedMethodClass);
                         }))
        {
            return doLookup(interface->getClassName());
        }
    }

    llvm_unreachable("Method resolution unexpectedly failed");
}

} // namespace

jllvm::VirtualMachine::VirtualMachine(BootOptions&& bootOptions)
    : m_classLoader(
        std::move(bootOptions.classPath),
        [this](ClassObject& classObject)
        {
            m_jit.add(&classObject);
            if (classObject.isInterface() || classObject.isAbstract())
            {
                return;
            }

            for (const ClassObject* curr : classObject.getSuperClasses())
            {
                for (const Method& iter : curr->getMethods())
                {
                    auto slot = iter.getTableSlot();
                    if (!slot)
                    {
                        continue;
                    }
                    classObject.getVTable()[*slot] = methodSelection(m_jit, &classObject, iter, curr);
                }
            }

            llvm::DenseMap<std::size_t, const jllvm::ClassObject*> idToInterface;
            for (const ClassObject* interface : classObject.getAllInterfaces())
            {
                idToInterface[interface->getInterfaceId()] = interface;
            }

            for (ITable* iTable : classObject.getITables())
            {
                const ClassObject* interface = idToInterface[iTable->getId()];
                for (const Method& iter : interface->getMethods())
                {
                    auto slot = iter.getTableSlot();
                    if (!slot)
                    {
                        continue;
                    }
                    iTable->getMethods()[*slot] = methodSelection(m_jit, &classObject, iter, interface);
                }
            }
        },
        [&] { return reinterpret_cast<void**>(m_gc.allocateStatic().data()); }),
      m_stringInterner(m_classLoader),
      m_jit(JIT::create(m_classLoader, m_gc, m_stringInterner, m_jniEnv.get(), bootOptions.executionMode)),
      m_interpreter(*this, /*enableOSR=*/bootOptions.executionMode != ExecutionMode::Interpreter),
      m_gc(/*random value for now*/ 1 << 20),
      // Seed from the C++ implementations entropy source.
      m_pseudoGen(std::random_device{}()),
      // Exclude 0 from the output as that is our sentinel value for "not yet calculated".
      m_hashIntDistrib(1, std::numeric_limits<std::uint32_t>::max()),
      m_javaHome(bootOptions.javaHome)
{
    m_jit.addImplementationSymbols(
        std::pair{"jllvm_gc_alloc", [&](std::uint32_t size) { return m_gc.allocate(size); }},
        std::pair{"jllvm_for_name_loaded",
                  [&](const char* name) { return m_classLoader.forNameLoaded(FieldType(name)); }},
        std::pair{"jllvm_instance_of",
                  [](const Object* object, const ClassObject* classObject) -> std::int32_t
                  { return object->instanceOf(classObject); }},
        std::pair{"jllvm_osr_frame_delete", [](const std::uint64_t* osrFrame) { delete[] osrFrame; }},
        std::pair{"jllvm_new_local_root", [&](Object* object) { return m_gc.root(object).release(); }},
        std::pair{"jllvm_throw", [&](Throwable* object) { throwJavaException(object); }},
        std::pair{"jllvm_initialize_class_object", [&](ClassObject* classObject)
                  {
                      // This should have been checked inline in LLVM IR.
                      assert(!classObject->isInitialized());
                      initialize(*classObject);
                  }},
        std::pair{"jllvm_build_class_cast_exception",
                  [&](Object* object, ClassObject* classObject) -> Object*
                  {
                      std::string className = object->getClass()->getDescriptor().pretty();
                      std::string name = classObject->getDescriptor().pretty();
                      llvm::StringRef prefix = classObject->isClass() || classObject->isInterface() ? "class " : "";

                      String* string = m_stringInterner.intern(
                          llvm::formatv("class {0} cannot be cast to {1}{2}", className, prefix, name).str());
                      GCUniqueRoot root =
                          m_gc.root(m_gc.allocate(&m_classLoader.forName("Ljava/lang/ClassCastException;")));
                      executeObjectConstructor(root, "(Ljava/lang/String;)V", string);
                      return root;
                  }},
        std::pair{"jllvm_build_null_pointer_exception",
                  [&]() -> Object*
                  {
                      GCUniqueRoot root =
                          m_gc.root(m_gc.allocate(&m_classLoader.forName("Ljava/lang/NullPointerException;")));
                      executeObjectConstructor(root, "()V");
                      return root;
                  }},
        std::pair{"jllvm_build_array_index_out_of_bounds_exception",
                  [&](std::int32_t index, std::int32_t size) -> Object*
                  {
                      String* string = m_stringInterner.intern(
                          llvm::formatv("Index {0} out of bounds for length {1}", index, size).str());
                      GCUniqueRoot root = m_gc.root(
                          m_gc.allocate(&m_classLoader.forName("Ljava/lang/ArrayIndexOutOfBoundsException;")));
                      executeObjectConstructor(root, "(Ljava/lang/String;)V", string);
                      return root;
                  }},
        std::pair{"jllvm_build_negative_array_size_exception",
                  [&](std::int32_t size) -> Object*
                  {
                      String* string = m_stringInterner.intern(std::to_string(size));
                      GCUniqueRoot root =
                          m_gc.root(m_gc.allocate(&m_classLoader.forName("Ljava/lang/NegativeArraySizeException;")));
                      executeObjectConstructor(root, "(Ljava/lang/String;)V", string);
                      return root;
                  }},
        std::pair{"jllvm_build_unsatisfied_link_error",
                  [&](Method* method) -> Object*
                  {
                      String* string = m_stringInterner.intern(method->prettySignature());
                      GCUniqueRoot root =
                          m_gc.root(m_gc.allocate(&m_classLoader.forName("Ljava/lang/UnsatisfiedLinkError;")));
                      executeObjectConstructor(root, "(Ljava/lang/String;)V", string);
                      return root;
                  }},
        std::pair{"jllvm_push_local_frame", [&] { m_gc.pushLocalFrame(); }},
        std::pair{"jllvm_pop_local_frame", [&] { m_gc.popLocalFrame(); }},
        std::pair{"jllvm_interpreter",
                  [&](const Method* method, std::uint16_t* byteCodeOffset, std::uint16_t* topOfStack,
                      std::uint64_t* operandStack, std::uint64_t* operandGCMask, std::uint64_t* localVariables,
                      std::uint64_t* localVariablesGCMask)
                  {
                      InterpreterContext context(*topOfStack, operandStack, operandGCMask, localVariables,
                                                 localVariablesGCMask);
                      return m_interpreter.executeMethod(*method, *byteCodeOffset, context);
                  }});

    registerJavaClasses(*this);

    m_gc.addRootObjectsProvider(
        [&](GarbageCollector::RootProvider::AddRootObjectFn addRootObjectFn)
        {
            for (ClassObject* classObject : m_classLoader.getLoadedClassObjects())
            {
                addRootObjectFn(classObject);
            }
        });

    initialize(m_classLoader.loadBootstrapClasses());

    m_stringInterner.loadStringClass();
    initialize(m_stringInterner.getStringClass());

    if (!bootOptions.systemInitialization)
    {
        return;
    }

    ClassObject& threadGroup = m_classLoader.forName("Ljava/lang/ThreadGroup;");
    initialize(threadGroup);
    m_mainThreadGroup = m_gc.allocate(&threadGroup);
    executeObjectConstructor(m_mainThreadGroup, "()V");

    ClassObject& thread = m_classLoader.forName("Ljava/lang/Thread;");
    initialize(thread);
    m_mainThread = m_gc.allocate(&thread);

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
    auto lookup = m_jit.lookup(classObject.getClassName(), "main", "([Ljava/lang/String;)V");
    if (!lookup)
    {
        llvm::report_fatal_error(("Failed to find main method due to " + toString(lookup.takeError())).c_str());
    }

    auto* javaArgs = m_gc.allocate<Array<String*>>(&m_classLoader.forName("[Ljava/lang/String;"), args.size());

    llvm::transform(args, javaArgs->begin(), [&](llvm::StringRef arg) { return m_stringInterner.intern(arg); });

    try
    {
        reinterpret_cast<void (*)(void*)>(lookup->getAddress())(javaArgs);
        return 0;
    }
    catch (const Throwable& activeException)
    {
        // TODO: Use printStackTrace:()V in the future

        // Equivalent to Throwable:toString() (does not yet work).
        std::string s = activeException.getClass()->getClassName().str();
        std::replace(s.begin(), s.end(), '/', '.');
        llvm::errs() << s;
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

    auto classInitializer = m_jit.lookup(classObject.getClassName(), "<clinit>", "()V");
    if (!classInitializer)
    {
        llvm::consumeError(classInitializer.takeError());
        return;
    }

    LLVM_DEBUG({
        llvm::dbgs() << "Executing class initializer "
                     << mangleDirectMethodCall(classObject.getClassName(), "<clinit>", "()V") << '\n';
    });
    reinterpret_cast<void (*)()>(classInitializer->getAddress())();
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
            for (const Code::ExceptionTable& exceptionTable : code->getExceptionTable())
            {
                if (exceptionTable.startPc > byteCodeOffset || byteCodeOffset >= exceptionTable.endPc)
                {
                    continue;
                }

                // Catch-all handlers as is used by 'finally' blocks don't have a catch type.
                if (!exceptionTable.catchType)
                {
                    handlerPc = exceptionTable.handlerPc;
                    break;
                }

                const ClassInfo* info = exceptionTable.catchType.resolve(classFile);
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
                    handlerPc = exceptionTable.handlerPc;
                    break;
                }
            }

            if (!handlerPc)
            {
                return;
            }

            m_jit.doExceptionOnStackReplacement(frame, *handlerPc, exception);
        });

    // If no Java frame is ready to handle the exception, unwind all of it completely.
    // The caller of Javas main or the start of a Java thread will catch this in C++ code.
    throw *exception;
}

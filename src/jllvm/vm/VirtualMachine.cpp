#include "VirtualMachine.hpp"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Support/Debug.h>

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
        // Lookup can and will fail if an abstract class declares the method as abstract. It is still
        // supposed to be found however, but the interface method is simply forced to be implemented by
        // derived classes.
        auto lookup = jit.lookup(className, resolvedMethod.getName(), resolvedMethod.getType());
        if (!lookup)
        {
            llvm::consumeError(lookup.takeError());
            return nullptr;
        }
        return reinterpret_cast<jllvm::VTableSlot>(lookup->getAddress());
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
        if (llvm::any_of(curr->getMethods(), [&](const jllvm::Method& method)
                { return !method.isStatic() && canOverride(method, curr, resolvedMethod, resolvedMethodClass); }))
        {
            return doLookup(curr->getClassName());
        }
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

jllvm::VirtualMachine::VirtualMachine(std::vector<std::string>&& classPath)
    : m_classLoader(
        std::move(classPath),
        [&](ClassObject* classObject)
        {
            if (auto classInitializer = m_jit.lookup(classObject->getClassName(), "<clinit>", "()V"))
            {
                LLVM_DEBUG({
                    llvm::dbgs() << "Executing class initializer "
                                 << mangleMethod(classObject->getClassName(), "<clinit>", "()V") << '\n';
                });
                reinterpret_cast<void (*)()>(classInitializer->getAddress())();
            }
            else
            {
                llvm::consumeError(classInitializer.takeError());
            }

            if (classObject->isInterface())
            {
                return;
            }

            for (const ClassObject* curr : classObject->getSuperClasses())
            {
                for (const Method& iter : curr->getMethods())
                {
                    auto slot = iter.getVTableSlot();
                    if (!slot)
                    {
                        continue;
                    }
                    classObject->getVTable()[*slot] = methodSelection(m_jit, classObject, iter, curr);
                }
            }

            llvm::DenseMap<std::size_t, const jllvm::ClassObject*> idToInterface;
            for (const ClassObject* interface : classObject->getAllInterfaces())
            {
                idToInterface[interface->getInterfaceId()] = interface;
            }

            for (ITable* iTable : classObject->getITables())
            {
                const ClassObject* interface = idToInterface[iTable->getId()];
                for (auto&& [index, method] : llvm::enumerate(llvm::make_filter_range(
                         interface->getMethods(), [](const Method& method) { return !method.isStatic(); })))
                {
                    iTable->getMethods()[index] = methodSelection(m_jit, classObject, method, interface);
                }
            }
        },
        [this](const ClassFile* classFile) { m_jit.add(classFile); },
        [&] { return reinterpret_cast<void**>(m_gc.allocateStatic().ref()); }),
      m_stringInterner(m_classLoader),
      m_gc(/*small random value for now*/ 4096),
      // Seed from the C++ implementations entropy source.
      m_pseudoGen(std::random_device{}()),
      // Exclude 0 from the output as that is our sentinel value for "not yet calculated".
      m_hashIntDistrib(1, std::numeric_limits<std::uint32_t>::max())
{
    registerJavaClasses(*this);

    m_classLoader.loadBootstrapClasses();
}

int jllvm::VirtualMachine::executeMain(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args)
{
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer)
    {
        llvm::report_fatal_error("Failed to open " + path);
    }

    const ClassObject& classObject = m_classLoader.addAndInitialize(std::move(*buffer));
    auto lookup = m_jit.lookup(classObject.getClassName(), "main", "([Ljava/lang/String;)V");
    if (!lookup)
    {
        llvm::report_fatal_error(("Failed to find main method due to " + toString(lookup.takeError())).c_str());
    }
    reinterpret_cast<void (*)(void*)>(lookup->getAddress())(nullptr);
    if (!m_activeException)
    {
        return 0;
    }

    // TODO: Use printStackTrace:()V in the future

    // Equivalent to Throwable:toString() (does not yet work).
    std::string s = m_activeException->getClass()->getClassName().str();
    std::replace(s.begin(), s.end(), '/', '.');
    llvm::errs() << s;
    if (m_activeException->detailMessage)
    {
        llvm::errs() << ": " << m_activeException->detailMessage->toUTF8();
    }
    llvm::errs() << '\n';

    return -1;
}

std::int32_t jllvm::VirtualMachine::createNewHashCode()
{
    return m_hashIntDistrib(m_pseudoGen);
}

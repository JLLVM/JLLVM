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

jllvm::VirtualMachine::VirtualMachine(std::vector<std::string>&& classPath)
    : m_classLoader(
        std::move(classPath),
        [this](const ClassFile* classFile, ClassObject& classObject)
        {
            m_jit.add(classFile);
            if (classObject.isInterface())
            {
                return;
            }

            for (const ClassObject* curr : classObject.getSuperClasses())
            {
                for (const Method& iter : curr->getMethods())
                {
                    auto slot = iter.getVTableSlot();
                    if (!slot)
                    {
                        continue;
                    }
                    classObject.getVTable()[*slot] = methodSelection(m_jit, &classObject, iter, curr);
                }
            }

            if (classObject.isAbstract())
            {
                return;
            }

            llvm::DenseMap<std::size_t, const jllvm::ClassObject*> idToInterface;
            for (const ClassObject* interface : classObject.getAllInterfaces())
            {
                idToInterface[interface->getInterfaceId()] = interface;
            }

            for (ITable* iTable : classObject.getITables())
            {
                const ClassObject* interface = idToInterface[iTable->getId()];
                for (auto&& [index, method] : llvm::enumerate(llvm::make_filter_range(
                         interface->getMethods(), [](const Method& method) { return !method.isStatic(); })))
                {
                    iTable->getMethods()[index] = methodSelection(m_jit, &classObject, method, interface);
                }
            }
        },
        [&] { return reinterpret_cast<void**>(m_gc.allocateStatic().data()); }),
      m_stringInterner(m_classLoader),
      m_gc(/*small random value for now*/ 4096),
      // Seed from the C++ implementations entropy source.
      m_pseudoGen(std::random_device{}()),
      // Exclude 0 from the output as that is our sentinel value for "not yet calculated".
      m_hashIntDistrib(1, std::numeric_limits<std::uint32_t>::max())
{
    m_jit.addImplementationSymbols(
        std::pair{"fmodf", &fmodf},
        std::pair{"jllvm_gc_alloc", [&](std::uint32_t size) { return m_gc.allocate(size); }},
        std::pair{"jllvm_for_name_loaded", [&](const char* name) { return m_classLoader.forNameLoaded(name); }},
        std::pair{"jllvm_instance_of",
                  [](const Object* object, const ClassObject* classObject) -> std::int32_t
                  { return object->instanceOf(classObject); }},
        std::pair{"activeException", m_activeException.data()},
        std::pair{"jllvm_new_local_root", [&](Object* object) { return m_gc.root(object).release(); }},
        std::pair{"jllvm_delete_local_root", [&](GCRootRef<Object> root)
                  {
                      auto* object = static_cast<Object*>(root);
                      m_gc.deleteRoot(root);
                      return object;
                  }},
        std::pair{"jllvm_initialize_class_object", [&](ClassObject* classObject)
                  {
                      // This should have been checked inline in LLVM IR.
                      assert(!classObject->isInitialized());
                      initialize(*classObject);
                  }},
        std::pair{"jllvm_build_class_cast_exception", [&](Object* object, ClassObject* classObject)
                  {
                      llvm::StringRef className = object->getClass()->getClassName();
                      llvm::StringRef prefix;
                      std::string name;
                      if (classObject->isClass() || classObject->isInterface())
                      {
                          prefix = "class";
                          name = classObject->getClassName();
                      }
                      else
                      {
                          // TODO: Pretty print array
                      }
                      String* string = m_stringInterner.intern(
                          llvm::formatv("class {0} cannot be cast to {1} {2}", className, prefix, name).str());
                      GCUniqueRoot root =
                          m_gc.root(m_gc.allocate(&m_classLoader.forName("Ljava/lang/ClassCastException;")));
                      executeObjectConstructor(root, "(Ljava/lang/String;)V", string);
                      return static_cast<Object*>(root);
                  }});

    registerJavaClasses(*this);

    initialize(m_classLoader.loadBootstrapClasses());

    m_stringInterner.loadStringClass();
    initialize(m_stringInterner.getStringClass());
}

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

void jllvm::VirtualMachine::initialize(ClassObject& classObject)
{
    if (classObject.isInitialized())
    {
        return;
    }

    classObject.setInitialized(true);

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
        llvm::dbgs() << "Executing class initializer " << mangleMethod(classObject.getClassName(), "<clinit>", "()V")
                     << '\n';
    });
    reinterpret_cast<void (*)()>(classInitializer->getAddress())();
}

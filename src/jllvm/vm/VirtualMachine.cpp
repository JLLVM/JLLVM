#include "VirtualMachine.hpp"

#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "jvm"

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

            for (const ClassObject* curr = classObject; curr; curr = curr->getSuperClass())
            {
                for (const Method& iter : curr->getMethods())
                {
                    if (iter.isAbstract())
                    {
                        continue;
                    }

                    auto slot = iter.getVTableSlot();
                    if (!slot)
                    {
                        continue;
                    }
                    if (!classObject->getVTable()[*slot])
                    {
                        classObject->getVTable()[*slot] = reinterpret_cast<VTableSlot>(
                            llvm::cantFail(m_jit.lookup(curr->getClassName(), iter.getName(), iter.getType()))
                                .getAddress());
                    }
                }
            }
        },
        [this](const ClassFile* classFile) { m_jit.add(classFile); }, [&] { return m_gc.allocateStatic(); }),
      m_gc(/*small random value for now*/ 4096)
{
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
    return 0;
}

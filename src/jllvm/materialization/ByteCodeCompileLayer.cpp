#include "ByteCodeCompileLayer.hpp"

#include <llvm/IR/Verifier.h>

#include "ByteCodeCompileUtils.hpp"
#include "CodeGenerator.hpp"

#define DEBUG_TYPE "jvm"

void jllvm::ByteCodeCompileLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                       const MethodInfo* methodInfo, const ClassFile* classFile)
{
    std::string methodName = mangleMethod(*methodInfo, *classFile);
    LLVM_DEBUG({ llvm::dbgs() << "Emitting LLVM IR for " << methodName << '\n'; });

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(methodName, *context);

    MethodType descriptor = parseMethodType(methodInfo->getDescriptor(*classFile));

    auto* function =
        llvm::Function::Create(descriptorToType(descriptor, methodInfo->isStatic(), module->getContext()),
                               llvm::GlobalValue::ExternalLinkage, mangleMethod(*methodInfo, *classFile), module.get());
    function->setGC("coreclr");
#ifdef LLVM_ADDRESS_SANITIZER_BUILD
    function->addFnAttr(llvm::Attribute::SanitizeAddress);
#endif

    auto code = methodInfo->getAttributes().find<Code>();
    assert(code);
    CodeGenerator codeGenerator{function,
                                *classFile,
                                LazyClassLoaderHelper(m_classLoader, m_mainDylib, m_stubsImplDylib, *m_stubsManager,
                                                      m_callbackManager, m_baseLayer, m_interner, m_dataLayout),
                                m_stringInterner,
                                descriptor,
                                code->getMaxStack(),
                                code->getMaxLocals()};

    codeGenerator.generateCode(*code);

    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

#ifndef NDEBUG
    if (llvm::verifyModule(*module, &llvm::dbgs()))
    {
        std::abort();
    }
#endif

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

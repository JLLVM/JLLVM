#include "JNIImplementationLayer.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <jllvm/class/Descriptors.hpp>

#include "ByteCodeCompileUtils.hpp"

namespace
{
std::string escape(llvm::StringRef string)
{
    std::string result;
    result.reserve(string.size());
    // TODO: Support for UTF-16 code units escaping.
    for (char iter : string)
    {
        switch (iter)
        {
            case '/': result += '_'; break;
            case '_': result += "_1"; break;
            case ';': result += "_2"; break;
            case '[': result += "_3"; break;
            default: result += iter; break;
        }
    }
    return result;
}

} // namespace

void jllvm::JNIImplementationLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                         const jllvm::MethodInfo* methodInfo, const jllvm::ClassFile* classFile)
{
    // Things that should happen here:
    // 1. Materialize a stub calling a compile callback to be called when the native method is called.
    // 2. Within the callback we need to materialize a trampoline function using LLVM code that does pre-setup for
    //    things like synchronization and possibly in the future GC rooting, creates a JNIENV* to prepend to the
    //    arguments and after the real implementation call, does post-setup for things like synchronization
    //    and exceptions. Within the trampoline materialization we should also look up the real implementation
    //    in 'm_jniImpls' trying the short signature first and the overloaded one second.
    // 3. We replace the stub with the materialized trampoline.

    llvm::orc::SymbolFlagsMap map = mr->getSymbols();

    std::string key = mangleMethod(*methodInfo, *classFile);
    llvm::cantFail(m_stubsManager->createStub(
        key,
        llvm::cantFail(m_callbackManager.getCompileCallback(
            [=]
            {
                // Reference:
                // https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#resolving-native-method-names
                std::string jniName =
                    "Java_" + escape(classFile->getThisClass()) + "_" + escape(methodInfo->getName(*classFile));

                auto lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, m_interner(jniName));
                if (!lookup)
                {
                    llvm::consumeError(lookup.takeError());
                    jniName += "__";
                    // Append just the parameters from the method descriptor. This is essentially dropping the return
                    // type and the parentheses.
                    jniName +=
                        methodInfo->getDescriptor(*classFile).drop_front(1).take_while([](char c) { return c != ')'; });
                    lookup = m_jniImpls.getExecutionSession().lookup({&m_jniImpls}, m_interner(jniName));
                    if (!lookup)
                    {
                        // TODO: Return callback throwing UnsatisfiedLinkError. Don't forget to update the stubs
                        //  manager to point to that callback too!
                        llvm::report_fatal_error(lookup.takeError());
                    }
                }

                std::string bridgeName = key;

                auto context = std::make_unique<llvm::LLVMContext>();
                auto module = std::make_unique<llvm::Module>(bridgeName, *context);
                module->setDataLayout(m_dataLayout);
                module->setTargetTriple(LLVM_HOST_TRIPLE);

                MethodType methodType = parseMethodType(methodInfo->getDescriptor(*classFile));
                auto* function = llvm::Function::Create(descriptorToType(methodType, methodInfo->isStatic(), *context),
                                                        llvm::GlobalValue::ExternalLinkage, bridgeName, module.get());
                llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", function));

                llvm::Value* environment = builder.CreateIntToPtr(
                    builder.getInt64(reinterpret_cast<std::uintptr_t>(m_jniNativeFunctions)), builder.getPtrTy());

                // TODO: Pre-setup code here

                llvm::SmallVector<llvm::Value*> args{environment};
                if (methodInfo->isStatic())
                {
                    // TODO: get class object and insert here.
                    args.push_back(llvm::ConstantPointerNull::get(referenceType(*context)));
                }
                for (llvm::Argument& arg : function->args())
                {
                    args.push_back(&arg);
                }

                llvm::Type* returnType = descriptorToType(methodType.returnType, *context);
                llvm::SmallVector<llvm::Type*> argTypes;
                // Env
                argTypes.push_back(environment->getType());
                // jclass or object
                argTypes.push_back(referenceType(*context));
                for (auto& iter : methodType.parameters)
                {
                    argTypes.push_back(descriptorToType(iter, *context));
                }

                llvm::Value* callee =
                    builder.CreateIntToPtr(builder.getInt64(lookup->getAddress()), builder.getPtrTy());
                llvm::Value* result =
                    builder.CreateCall(llvm::FunctionType::get(returnType, argTypes, false), callee, args);

                // TODO: Post-setup code here

                if (returnType->isVoidTy())
                {
                    builder.CreateRetVoid();
                }
                else
                {
                    builder.CreateRet(result);
                }

                llvm::cantFail(
                    m_irLayer.add(m_jniBridges, llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

                llvm::JITTargetAddress bridgeMethod =
                    llvm::cantFail(m_jniBridges.getExecutionSession().lookup({&m_jniBridges}, m_interner(bridgeName)))
                        .getAddress();

                llvm::cantFail(m_stubsManager->updatePointer(key, bridgeMethod));

                return bridgeMethod;
            })),
        map.begin()->second));

    assert(map.size() == 1 && "ByteCodeLayer only ever defines one method");
    llvm::cantFail(
        mr->replace(llvm::orc::absoluteSymbols({{map.begin()->first, m_stubsManager->findStub(key, false)}})));
}

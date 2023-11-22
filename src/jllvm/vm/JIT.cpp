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

#include "JIT.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ExecutionEngine/JITLink/EHFrameSupport.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Scalar/RewriteStatepointsForGC.h>

#include <jllvm/compiler/Compiler.hpp>
#include <jllvm/llvm/ClassObjectStubImportPass.hpp>
#include <jllvm/llvm/MarkSanitizersGCLeafs.hpp>
#include <jllvm/materialization/ClassObjectStubDefinitionsGenerator.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <utility>

#include "StackMapRegistrationPlugin.hpp"

#define DEBUG_TYPE "jvm"

namespace
{

/// Custom 'EHFrameRegistrar' which registers the 'eh_frame' sections in our unwinder. This is very similar to
/// 'llvm::jitlink::InProcessEHFrameRegistrar' except that the latter hardcodes the use of either 'libgcc' or
/// 'libunwind' based on what LLVM was built with. Since LLVM is almost certainly built with 'libgcc' on Linux, we have
/// to provide our own implementation that can work with 'libunwind'.
class EHRegistration : public llvm::jitlink::EHFrameRegistrar
{
public:
    llvm::Error registerEHFrames(llvm::orc::ExecutorAddrRange EHFrameSection) override
    {
        jllvm::registerEHSection({EHFrameSection.Start.toPtr<const char*>(), EHFrameSection.size()});
        return llvm::Error::success();
    }

    llvm::Error deregisterEHFrames(llvm::orc::ExecutorAddrRange EHFrameSection) override
    {
        jllvm::deregisterEHSection({EHFrameSection.Start.toPtr<const char*>(), EHFrameSection.size()});
        return llvm::Error::success();
    }
};

} // namespace

jllvm::JIT::JIT(std::unique_ptr<llvm::orc::ExecutionSession>&& session,
                std::unique_ptr<llvm::orc::EPCIndirectionUtils>&& epciu, llvm::orc::JITTargetMachineBuilder&& builder,
                llvm::DataLayout&& layout, ClassLoader& classLoader, GarbageCollector& gc,
                StringInterner& stringInterner, void* jniFunctions)
    : m_session(std::move(session)),
      m_externalStubs(llvm::cantFail(m_session->createJITDylib("<stubs>"))),
      m_javaJITSymbols(llvm::cantFail(m_session->createJITDylib("<javaJIT>"))),
      m_javaJITImplDetails(llvm::cantFail(m_session->createJITDylib("<implementation>"))),
      m_epciu(std::move(epciu)),
      m_targetMachine(llvm::cantFail(builder.createTargetMachine())),
      m_lazyCallThroughManager(m_epciu->getLazyCallThroughManager()),
      m_externalStubsManager(m_epciu->createIndirectStubsManager()),
      m_classLoader(classLoader),
      m_stringInterner(stringInterner),
      m_dataLayout(layout),
      m_interner(*m_session, m_dataLayout),
      m_objectLayer(*m_session),
      m_compilerLayer(*m_session, m_objectLayer, std::make_unique<llvm::orc::SimpleCompiler>(*m_targetMachine)),
      m_optimizeLayer(*m_session, m_compilerLayer,
                      [&](llvm::orc::ThreadSafeModule tsm, const llvm::orc::MaterializationResponsibility&)
                      {
                          tsm.withModuleDo([&](llvm::Module& module) { optimize(module); });
                          return std::move(tsm);
                      }),
      m_byteCodeCompileLayer(stringInterner, m_optimizeLayer, m_interner, m_dataLayout),
      m_jniLayer(*m_session, m_interner, m_optimizeLayer, m_dataLayout, jniFunctions)
{
    // JITted Java methods mustn't lookup symbols within 'm_javaJITSymbols', as these are always JITted methods, but
    // rather resolve direct method calls to the stubs in 'm_externalStubs'. All other required symbols are in the
    // dylib for implementation details.
    m_javaJITSymbols.setLinkOrder({{&m_externalStubs, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
                                   {&m_javaJITImplDetails, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}},
                                  /*LinkAgainstThisJITDylibFirst=*/false);

    m_javaJITSymbols.withLinkOrderDo(
        [&](const llvm::orc::JITDylibSearchOrder& searchOrder)
        {
            // The functions created by the stub class object stub definitions generator are also considered an
            // implementation detail and may only link against the stubs.
            m_javaJITImplDetails.addGenerator(std::make_unique<ClassObjectStubDefinitionsGenerator>(
                *m_externalStubsManager, m_optimizeLayer, m_dataLayout, searchOrder, classLoader));
        });

    m_objectLayer.addPlugin(std::make_unique<llvm::orc::DebugObjectManagerPlugin>(
        *m_session, std::make_unique<llvm::orc::EPCDebugObjectRegistrar>(
                        *m_session, llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper))));
    // Register unwind info in both our forked libunwind and the platform implementation.
    m_objectLayer.addPlugin(
        std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(*m_session, std::make_unique<EHRegistration>()));
    m_objectLayer.addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
        *m_session, std::make_unique<llvm::jitlink::InProcessEHFrameRegistrar>()));

    m_objectLayer.addPlugin(std::make_unique<StackMapRegistrationPlugin>(gc, m_javaFrames));

#if LLVM_ADDRESS_SANITIZER_BUILD
    m_javaJITImplDetails.addGenerator(llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        m_dataLayout.getGlobalPrefix(),
        /*Allow=*/[](const llvm::orc::SymbolStringPtr& symbolStringPtr)
        { return (*symbolStringPtr).starts_with("__asan"); })));
#endif
}

jllvm::JIT jllvm::JIT::create(ClassLoader& classLoader, GarbageCollector& gc, StringInterner& stringInterner,
                              void* jniFunctions)
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto epc = llvm::cantFail(llvm::orc::SelfExecutorProcessControl::Create());
    auto es = std::make_unique<llvm::orc::ExecutionSession>(std::move(epc));
    auto epciu = llvm::cantFail(llvm::orc::EPCIndirectionUtils::Create(es->getExecutorProcessControl()));
    epciu->createLazyCallThroughManager(*es, llvm::pointerToJITTargetAddress(+[]
                                                                             {
                                                                                 // TODO: throw UnsatisfiedLinkError
                                                                                 // exception.
                                                                                 llvm::report_fatal_error(
                                                                                     "Dynamic linking failed");
                                                                             }));

    llvm::cantFail(llvm::orc::setUpInProcessLCTMReentryViaEPCIU(*epciu));

    auto jtmb = llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost());
    jtmb.getOptions().EmulatedTLS = false;
    jtmb.getOptions().ExceptionModel = llvm::ExceptionHandling::DwarfCFI;
    jtmb.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);
    auto dl = llvm::cantFail(jtmb.getDefaultDataLayoutForTarget());
    return JIT(std::move(es), std::move(epciu), std::move(jtmb), std::move(dl), classLoader, gc, stringInterner,
               jniFunctions);
}

jllvm::JIT::~JIT()
{
    llvm::cantFail(m_session->endSession());
    llvm::cantFail(m_epciu->cleanup());
}

void jllvm::JIT::add(const ClassObject* classObject)
{
    llvm::orc::IndirectStubsManager::StubInitsMap stubInits;
    llvm::orc::SymbolMap methodGlobals;
    for (const Method& method : classObject->getMethods())
    {
        if (method.isAbstract())
        {
            continue;
        }

        // Register the method in the JIT symbol table in case any code references it. This is done for methods as
        // there is exactly one symbol per method but not for class objects, as there are infinitely many class objects
        // due to being able to create array class objects of other class objects.
        // Class objects are therefore created on demand in 'ClassObjectStubDefinitionsGenerator'.
        methodGlobals[m_interner(mangleMethodGlobal(&method))] = llvm::JITEvaluatedSymbol::fromPointer(&method);

        std::string symbolName = mangleDirectMethodCall(&method);
        llvm::orc::SymbolStringPtr mangledSymbol = m_interner(symbolName);
        LLVM_DEBUG({ llvm::dbgs() << "Adding " << symbolName << " to JIT Link graph\n"; });

        // Register the Java method in the corresponding layer.
        if (method.isNative())
        {
            llvm::cantFail(m_jniLayer.add(m_javaJITSymbols, &method));
        }
        else
        {
            llvm::cantFail(m_byteCodeCompileLayer.add(m_javaJITSymbols, &method));
        }

        // Create a stub entry for this method. Right now, we by default create a trampoline which upon being called
        // will lookup the method within 'm_javaJITSymbols'.
        stubInits[symbolName] = {llvm::cantFail(m_lazyCallThroughManager.getCallThroughTrampoline(
                                     m_javaJITSymbols, mangledSymbol,
                                     [=, this](llvm::JITTargetAddress executorAddr)
                                     {
                                         // After having compiled and resolved the method, update the stub to point
                                         // to the resolved method instead.
                                         return m_externalStubsManager->updatePointer(symbolName, executorAddr);
                                     })),
                                 llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    }

    if (stubInits.empty())
    {
        return;
    }

    // Define the methods in the implementation details dylib.
    llvm::cantFail(m_javaJITImplDetails.define(llvm::orc::absoluteSymbols(std::move(methodGlobals))));

    // Create the stubs and define them with the direct method call mangling in the external stubs dylib.
    llvm::cantFail(m_externalStubsManager->createStubs(stubInits));

    llvm::orc::SymbolMap methods;
    for (llvm::StringRef stubName : llvm::map_range(stubInits, [](auto&& entry) { return entry.first(); }))
    {
        methods[m_interner(stubName)] = m_externalStubsManager->findStub(stubName, /*ExportedStubOnly=*/true);
    }

    llvm::cantFail(m_externalStubs.define(llvm::orc::absoluteSymbols(std::move(methods))));
}

void jllvm::JIT::optimize(llvm::Module& module)
{
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    llvm::PipelineTuningOptions options;
    options.LoopInterleaving = true;
    options.LoopUnrolling = true;
    options.LoopVectorization = true;
    options.SLPVectorization = true;
    options.MergeFunctions = true;
    llvm::PassBuilder passBuilder(m_targetMachine.get(), options, std::nullopt);

    passBuilder.registerPipelineStartEPCallback(
        [&](llvm::ModulePassManager& modulePassManager, llvm::OptimizationLevel)
        { modulePassManager.addPass(ClassObjectStubImportPass{m_classLoader}); });

    passBuilder.registerOptimizerLastEPCallback(
        [&](llvm::ModulePassManager& modulePassManager, llvm::OptimizationLevel)
        {
#if LLVM_ADDRESS_SANITIZER_BUILD
            llvm::AddressSanitizerOptions options;
            modulePassManager.addPass(llvm::AddressSanitizerPass(options));
            modulePassManager.addPass(llvm::RequireAnalysisPass<llvm::GlobalsAA, llvm::Module>{});
            modulePassManager.addPass(MarkSanitizersGCLeafsPass{});
#endif
            modulePassManager.addPass(llvm::RewriteStatepointsForGC{});
        });

    fam.registerPass([&] { return passBuilder.buildDefaultAAPipeline(); });
    passBuilder.registerModuleAnalyses(mam);
    passBuilder.registerCGSCCAnalyses(cgam);
    passBuilder.registerFunctionAnalyses(fam);
    passBuilder.registerLoopAnalyses(lam);
    passBuilder.crossRegisterProxies(lam, fam, cgam, mam);

    auto mpm = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    mpm.run(module, mam);
}

llvm::SmallVector<std::uint64_t> jllvm::JIT::readLocals(const UnwindFrame& frame) const
{
    llvm::ArrayRef<FrameValue<std::uint64_t>> locals =
        getJavaMethodMetadata(frame.getFunctionPointer())->getJITData()[frame.getProgramCounter()].locals;

    return llvm::to_vector(
        llvm::map_range(locals, [&](FrameValue<std::uint64_t> frameValue) { return frameValue.readScalar(frame); }));
}

void jllvm::JIT::doExceptionOnStackReplacement(const UnwindFrame& frame, std::uint16_t byteCodeOffset,
                                               Throwable* exception)
{
    llvm::SmallVector<std::uint64_t> locals = readLocals(frame);

    // Dynamically allocating memory here as this frame will be replaced at the end of this method and all local
    // variables destroyed. The OSR method will delete the array on entry once no longer needed.
    auto* operandPtr = new std::uint64_t[locals.size() + 1];
    *operandPtr = reinterpret_cast<std::uint64_t>(exception);
    std::uint64_t* localsPtr = operandPtr + 1;
    llvm::copy(locals, localsPtr);

    // Lookup the OSR version if it has already been compiled. If not, compile and add it now.
    const Method& method = *getJavaMethodMetadata(frame.getFunctionPointer())->getMethod();
    llvm::orc::SymbolStringPtr mangledName = m_interner(mangleOSRMethod(&method, byteCodeOffset));
    llvm::Expected<llvm::JITEvaluatedSymbol> osrMethod = m_session->lookup({&m_javaJITSymbols}, mangledName);
    if (!osrMethod)
    {
        llvm::consumeError(osrMethod.takeError());
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("name", *context);

        module->setDataLayout(m_dataLayout);
        module->setTargetTriple(LLVM_HOST_TRIPLE);

        compileOSRMethod(*module, byteCodeOffset, method, m_stringInterner);

        llvm::cantFail(
            m_optimizeLayer.add(m_javaJITSymbols, llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

        osrMethod = m_session->lookup({&m_javaJITSymbols}, mangledName);
    }

    frame.resumeExecutionAtFunction(
        reinterpret_cast<void (*)(std::uint64_t*, std::uint64_t*)>(llvm::cantFail(std::move(osrMethod)).getAddress()),
        operandPtr, localsPtr);
}

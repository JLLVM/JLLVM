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

#include <jllvm/materialization/ClassObjectStubDefinitionsGenerator.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <utility>

#include "ClassObjectStubImportPass.hpp"
#include "MarkSanitizersGCLeafs.hpp"
#include "StackMapRegistrationPlugin.hpp"

#define DEBUG_TYPE "jvm"

namespace
{
class JavaFrameRegistrationPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
{
    llvm::DenseSet<void*>& m_javaFrameSet;

public:
    explicit JavaFrameRegistrationPlugin(llvm::DenseSet<void*>& javaFrameSet) : m_javaFrameSet(javaFrameSet) {}

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override
    {
        return llvm::Error::success();
    }

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override
    {
        return llvm::Error::success();
    }

    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey, llvm::orc::ResourceKey) override {}

    void modifyPassConfig(llvm::orc::MaterializationResponsibility&, llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override
    {
        config.PostAllocationPasses.emplace_back(
            [&](llvm::jitlink::LinkGraph& g)
            {
                std::string sectionName = "java";
                if (llvm::Triple(LLVM_HOST_TRIPLE).isOSBinFormatMachO())
                {
                    sectionName = "__TEXT," + sectionName;
                }

                llvm::jitlink::Section* section = g.findSectionByName(sectionName);
                if (!section)
                {
                    return llvm::Error::success();
                }
                for (llvm::jitlink::Symbol* iter : section->symbols())
                {
                    m_javaFrameSet.insert(iter->getAddress().toPtr<void*>());
                }

                return llvm::Error::success();
            });
    }
};

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
      m_main(llvm::cantFail(m_session->createJITDylib("<main>"))),
      m_implementation(llvm::cantFail(m_session->createJITDylib("<implementation>"))),
      m_epciu(std::move(epciu)),
      m_targetMachine(llvm::cantFail(builder.createTargetMachine())),
      m_callbackManager(llvm::cantFail(llvm::orc::createLocalCompileCallbackManager(
          llvm::Triple(LLVM_HOST_TRIPLE), *m_session,
          llvm::pointerToJITTargetAddress(+[] { llvm::report_fatal_error("Callback failed"); })))),
      m_classLoader(classLoader),
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
      m_byteCodeOnDemandLayer(m_byteCodeCompileLayer, *m_session, m_interner,
                              llvm::orc::createLocalIndirectStubsManagerBuilder(llvm::Triple(LLVM_HOST_TRIPLE)),
                              m_epciu->getLazyCallThroughManager()),
      m_jniLayer(*m_session, m_epciu->createIndirectStubsManager(), *m_callbackManager, m_interner, m_optimizeLayer,
                 m_dataLayout, jniFunctions, m_implementation)
{
    m_main.addToLinkOrder(m_implementation);
    m_main.addGenerator(std::make_unique<ClassObjectStubDefinitionsGenerator>(
        m_epciu->createIndirectStubsManager(), *m_callbackManager, m_optimizeLayer, m_dataLayout, m_main, classLoader));

    m_objectLayer.addPlugin(std::make_unique<llvm::orc::DebugObjectManagerPlugin>(
        *m_session, std::make_unique<llvm::orc::EPCDebugObjectRegistrar>(
                        *m_session, llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper))));
    // Register unwind info in both our forked libunwind and the platform implementation.
    m_objectLayer.addPlugin(
        std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(*m_session, std::make_unique<EHRegistration>()));
    m_objectLayer.addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
        *m_session, std::make_unique<llvm::jitlink::InProcessEHFrameRegistrar>()));

    m_objectLayer.addPlugin(std::make_unique<StackMapRegistrationPlugin>(gc));
    m_objectLayer.addPlugin(std::make_unique<JavaFrameRegistrationPlugin>(m_javaFrames));

#if LLVM_ADDRESS_SANITIZER_BUILD
    m_implementation.addGenerator(llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
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

void jllvm::JIT::add(const jllvm::ClassFile* classFile, const ClassObject* classObject)
{
    for (auto&& [info, method] : llvm::zip(classFile->getMethods(), classObject->getMethods()))
    {
        if (info.isAbstract())
        {
            continue;
        }

        LLVM_DEBUG({ llvm::dbgs() << "Adding " << mangleDirectMethodCall(&method) << " to JIT Link graph\n"; });

        if (info.isNative())
        {
            llvm::cantFail(m_jniLayer.add(m_main, &info, classFile, &method, classObject));
            continue;
        }

        llvm::cantFail(m_byteCodeOnDemandLayer.add(m_main, &info, classFile, &method, classObject));
    }
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

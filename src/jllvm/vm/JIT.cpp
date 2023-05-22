#include "JIT.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ExecutionEngine/JITLink/EHFrameSupport.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Scalar/RewriteStatepointsForGC.h>

#include <jllvm/materialization/LambdaMaterialization.hpp>

#include <utility>

#include "StackMapRegistrationPlugin.hpp"

#define DEBUG_TYPE "jvm"

jllvm::JIT::JIT(std::unique_ptr<llvm::orc::ExecutionSession>&& session,
                std::unique_ptr<llvm::orc::EPCIndirectionUtils>&& epciu, llvm::orc::JITTargetMachineBuilder&& builder,
                llvm::DataLayout&& layout, ClassLoader& classLoader, GarbageCollector& gc,
                StringInterner& stringInterner, void* jniFunctions)
    : m_session(std::move(session)),
      m_main(llvm::cantFail(m_session->createJITDylib("<main>"))),
      m_epciu(std::move(epciu)),
      m_targetMachine(llvm::cantFail(builder.createTargetMachine())),
      m_callbackManager(llvm::cantFail(llvm::orc::createLocalCompileCallbackManager(
          llvm::Triple(LLVM_HOST_TRIPLE), *m_session,
          llvm::pointerToJITTargetAddress(+[] { llvm::report_fatal_error("Callback failed"); })))),
      m_dataLayout(layout),
      m_classLoader(classLoader),
      m_stringInterner(stringInterner),
      m_interner(*m_session, m_dataLayout),
      m_objectLayer(*m_session),
      m_compilerLayer(*m_session, m_objectLayer, std::make_unique<llvm::orc::ConcurrentIRCompiler>(builder)),
      m_optimizeLayer(*m_session, m_compilerLayer,
                      [&](llvm::orc::ThreadSafeModule tsm, const llvm::orc::MaterializationResponsibility&)
                      {
                          tsm.withModuleDo([&](llvm::Module& module) { optimize(module); });
                          return std::move(tsm);
                      }),
      m_byteCodeCompileLayer(m_classLoader, m_stringInterner, m_main, m_epciu->createIndirectStubsManager(),
                             *m_callbackManager, m_optimizeLayer, m_interner, m_dataLayout),
      m_byteCodeOnDemandLayer(m_byteCodeCompileLayer, *m_session, m_interner,
                              llvm::orc::createLocalIndirectStubsManagerBuilder(llvm::Triple(LLVM_HOST_TRIPLE)),
                              m_epciu->getLazyCallThroughManager()),
      m_jniLayer(*m_session, m_epciu->createIndirectStubsManager(), *m_callbackManager, m_interner, m_optimizeLayer,
                 m_dataLayout, jniFunctions),
      m_gc(gc)
{
    m_objectLayer.addPlugin(std::make_unique<llvm::orc::DebugObjectManagerPlugin>(
        *m_session, std::make_unique<llvm::orc::EPCDebugObjectRegistrar>(
                        *m_session, llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper))));
    m_objectLayer.addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
        *m_session, std::make_unique<llvm::jitlink::InProcessEHFrameRegistrar>()));
    m_objectLayer.addPlugin(std::make_unique<StackMapRegistrationPlugin>(m_gc));

    llvm::cantFail(m_main.define(createLambdaMaterializationUnit(
        "jllvm_gc_alloc", m_optimizeLayer, [&](std::uint32_t size) { return m_gc.allocate(size); }, m_dataLayout,
        m_interner)));
    llvm::cantFail(m_main.define(llvm::orc::absoluteSymbols(
        {{m_interner("jllvm_instance_of"), llvm::JITEvaluatedSymbol::fromPointer(
                                               +[](const Object* object, const ClassObject* classObject) -> std::int32_t
                                               { return object->instanceOf(classObject); })},
         {m_interner("fmodf"), llvm::JITEvaluatedSymbol::fromPointer(fmodf)}})));
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

void jllvm::JIT::add(const jllvm::ClassFile* classFile)
{
    for (const MethodInfo& iter : classFile->getMethods())
    {
        if (iter.isAbstract())
        {
            continue;
        }

        LLVM_DEBUG({ llvm::dbgs() << "Adding " << mangleMethod(iter, *classFile) << " to JIT Link graph\n"; });

        if (iter.isNative())
        {
            llvm::cantFail(m_jniLayer.add(m_main, &iter, classFile));
            continue;
        }

        llvm::cantFail(m_byteCodeOnDemandLayer.add(m_main, &iter, classFile));
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

    passBuilder.registerOptimizerLastEPCallback([&](llvm::ModulePassManager& modulePassManager, llvm::OptimizationLevel)
                                                { modulePassManager.addPass(llvm::RewriteStatepointsForGC{}); });

    fam.registerPass([&] { return passBuilder.buildDefaultAAPipeline(); });
    passBuilder.registerModuleAnalyses(mam);
    passBuilder.registerCGSCCAnalyses(cgam);
    passBuilder.registerFunctionAnalyses(fam);
    passBuilder.registerLoopAnalyses(lam);
    passBuilder.crossRegisterProxies(lam, fam, cgam, mam);

    auto mpm = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    mpm.run(module, mam);
}

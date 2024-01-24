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

#include "Runtime.hpp"

#include <llvm/ADT/ScopeExit.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/ExecutionEngine/JITLink/EHFrameSupport.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Scalar/RewriteStatepointsForGC.h>

#include <jllvm/llvm/ClassObjectStubImportPass.hpp>
#include <jllvm/llvm/MarkSanitizersGCLeafs.hpp>
#include <jllvm/materialization/ClassObjectDefinitionsGenerator.hpp>

#include "StackMapRegistrationPlugin.hpp"
#include "VirtualMachine.hpp"

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

#ifdef __APPLE__
extern "C" void __bzero();
#endif

jllvm::Runtime::Runtime(VirtualMachine& virtualMachine, llvm::ArrayRef<Executor*> executors)
    : m_session(std::make_unique<llvm::orc::ExecutionSession>(
          llvm::cantFail(llvm::orc::SelfExecutorProcessControl::Create()))),
      m_executors(executors.begin(), executors.end()),
      m_jitCCStubs(m_session->createBareJITDylib("<jitCCStubs>")),
      m_interpreterCCStubs(m_session->createBareJITDylib("<interpreterCCStubs>")),
      m_classAndMethodObjects(m_session->createBareJITDylib("<class-and-method-objects>")),
      m_clib(m_session->createBareJITDylib("<clib>")),
      m_epciu(llvm::cantFail(llvm::orc::EPCIndirectionUtils::Create(m_session->getExecutorProcessControl()))),
      m_targetMachine(
          []
          {
              auto jtmb = llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost());
              jtmb.getOptions().EmulatedTLS = false;
              jtmb.getOptions().ExceptionModel = llvm::ExceptionHandling::DwarfCFI;
              jtmb.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);
              return llvm::cantFail(jtmb.createTargetMachine());
          }()),
      m_lazyCallThroughManager(m_epciu->createLazyCallThroughManager(
          *m_session, llvm::pointerToJITTargetAddress(+[] { llvm::report_fatal_error("Dynamic linking failed"); }))),
      m_jitCCStubsManager(m_epciu->createIndirectStubsManager()),
      m_interpreterCCStubsManager(m_epciu->createIndirectStubsManager()),
      m_dataLayout(m_targetMachine->createDataLayout()),
      m_interner(*m_session, m_dataLayout),
      m_classLoader(virtualMachine.getClassLoader()),
      m_objectLayer(*m_session),
      m_compilerLayer(*m_session, m_objectLayer, std::make_unique<llvm::orc::SimpleCompiler>(*m_targetMachine)),
      m_optimizeLayer(*m_session, m_compilerLayer,
                      [&](llvm::orc::ThreadSafeModule tsm, const llvm::orc::MaterializationResponsibility&)
                      {
                          tsm.withModuleDo([&](llvm::Module& module) { optimize(module); });
                          return std::move(tsm);
                      }),
      m_interpreter2JITLayer(m_optimizeLayer, m_interner, m_dataLayout)
{
    llvm::cantFail(llvm::orc::setUpInProcessLCTMReentryViaEPCIU(*m_epciu));

    m_objectLayer.addPlugin(std::make_unique<llvm::orc::DebugObjectManagerPlugin>(
        *m_session, std::make_unique<llvm::orc::EPCDebugObjectRegistrar>(
                        *m_session, llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper))));
    // Register unwind info in both our forked libunwind and the platform implementation.
    m_objectLayer.addPlugin(
        std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(*m_session, std::make_unique<EHRegistration>()));
    m_objectLayer.addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
        *m_session, std::make_unique<llvm::jitlink::InProcessEHFrameRegistrar>()));

    m_objectLayer.addPlugin(std::make_unique<StackMapRegistrationPlugin>(virtualMachine.getGC(), m_javaFrames));

    m_classAndMethodObjects.addGenerator(
        std::make_unique<ClassObjectDefinitionsGenerator>(m_classLoader, m_dataLayout));

    llvm::cantFail(m_clib.define(llvm::orc::absoluteSymbols({
        {m_interner("memset"), llvm::JITEvaluatedSymbol::fromPointer(memset)},
        {m_interner("memcpy"), llvm::JITEvaluatedSymbol::fromPointer(memcpy)},
        {m_interner("fmodf"), llvm::JITEvaluatedSymbol::fromPointer(fmodf)},
        {m_interner("fmod"), llvm::JITEvaluatedSymbol::fromPointer(static_cast<double (*)(double, double)>(fmod))},
#ifdef __APPLE__
        {m_interner("__bzero"), llvm::JITEvaluatedSymbol::fromPointer(::__bzero)},
#endif
    })));

#if LLVM_ADDRESS_SANITIZER_BUILD
    m_clib.addGenerator(llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        m_dataLayout.getGlobalPrefix(),
        /*Allow=*/[](const llvm::orc::SymbolStringPtr& symbolStringPtr)
        { return (*symbolStringPtr).starts_with("__asan"); })));
#endif
}

void jllvm::Runtime::add(ClassObject* classObject, Executor& defaultExecutor)
{
    llvm::orc::SymbolMap methodGlobals;

    llvm::orc::IndirectStubsManager::StubInitsMap jitStubInits;
    llvm::orc::IndirectStubsManager::StubInitsMap interpreterStubInits;
    for (const Method& method : classObject->getMethods())
    {
        if (method.isAbstract())
        {
            continue;
        }

        // Register the method in the dylib in case any code references it. This is done for methods as
        // there is exactly one symbol per method but not for class objects, as there are infinitely many class objects
        // due to being able to create array class objects of other class objects.
        // Class objects are therefore created on demand in 'ClassObjectDefinitionsGenerator'.
        methodGlobals[m_interner(mangleMethodGlobal(&method))] = llvm::JITEvaluatedSymbol::fromPointer(&method);

        for (Executor* executor : m_executors)
        {
            if (executor->canExecute(method))
            {
                executor->add(method);
            }
        }

        Executor* executor = &defaultExecutor;
        if (!executor->canExecute(method))
        {
            // If the default executor is not capable of executing the method, find the first one that does.
            auto iter = llvm::find_if(m_executors, [&](Executor* executor) { return executor->canExecute(method); });
            assert(iter != m_executors.end() && "executor capable of executing the method must exist");
            executor = *iter;
        }

        llvm::orc::SymbolAliasMap symbolAliasMap;
        m_executorState[&method] = executor;

        std::string name = mangleDirectMethodCall(&method);
        llvm::orc::SymbolStringPtr mangledName = m_interner(name);

        auto addStub = [&](llvm::orc::IndirectStubsManager::StubInitsMap& stubInitsMap,
                           llvm::orc::JITDylib& sourceDylib, llvm::orc::IndirectStubsManager& stubsManager)
        {
            stubInitsMap[name] = {llvm::cantFail(m_lazyCallThroughManager.getCallThroughTrampoline(
                                      sourceDylib, mangledName,
                                      [&stubsManager, name](llvm::JITTargetAddress executorAddr)
                                      {
                                          // After having compiled and resolved the method, update the stub to point
                                          // to the resolved method instead.
                                          return stubsManager.updatePointer(name, executorAddr);
                                      })),
                                  llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
        };
        addStub(jitStubInits, executor->getJITCCDylib(), *m_jitCCStubsManager);
        addStub(interpreterStubInits, executor->getInterpreterCCDylib(), *m_interpreterCCStubsManager);
    }

    auto defineStubs = [&](const llvm::orc::IndirectStubsManager::StubInitsMap& stubInitsMap,
                           llvm::orc::IndirectStubsManager& stubsManager, llvm::orc::JITDylib& dylib)
    {
        // Create the stubs and define them with the direct method call mangling in the stubs dylib.
        llvm::cantFail(stubsManager.createStubs(stubInitsMap));

        llvm::orc::SymbolMap methods;
        for (llvm::StringRef stubName : llvm::map_range(stubInitsMap, [](auto&& entry) { return entry.first(); }))
        {
            methods[m_interner(stubName)] = stubsManager.findStub(stubName, /*ExportedStubOnly=*/true);
        }

        llvm::cantFail(dylib.define(llvm::orc::absoluteSymbols(std::move(methods))));
    };

    // Define the methods in the dylib.
    llvm::cantFail(m_classAndMethodObjects.define(llvm::orc::absoluteSymbols(std::move(methodGlobals))));
    defineStubs(jitStubInits, *m_jitCCStubsManager, m_jitCCStubs);
    defineStubs(interpreterStubInits, *m_interpreterCCStubsManager, m_interpreterCCStubs);

    prepare(*classObject);
}

void jllvm::Runtime::optimize(llvm::Module& module)
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

jllvm::Runtime::~Runtime()
{
    llvm::cantFail(m_session->endSession());
    llvm::cantFail(m_epciu->cleanup());
}

void jllvm::Runtime::prepare(ClassObject& classObject)
{
    // Lookup the JIT CC and interpreter CC implementations once ahead of time and save them in the method object.
    // This is not only more convenient (allowing the method object to be called standalone), but is likely also faster
    // on average as we do not have to do any repeated lookups and can batch the lookups here.
    llvm::DenseMap<llvm::orc::SymbolStringPtr, Method*> methodMapping(classObject.getMethods().keys().size());
    for (const Method& method : classObject.getMethods())
    {
        if (method.isAbstract())
        {
            // Abstract methods are not callable.
            continue;
        }
        methodMapping.insert({m_interner(mangleDirectMethodCall(&method)), const_cast<Method*>(&method)});
    }

    // We perform the lookup asynchronously (purely because we can). Use two promises to ensure that both lookups are
    // done prior to exiting the method.
    std::promise<void> jitLookupDone;
    std::promise<void> interpreterLookupDone;
    auto waitPreparedOnExit = llvm::make_scope_exit(
        [jitFuture = jitLookupDone.get_future(), interpreterFuture = interpreterLookupDone.get_future()]
        {
            jitFuture.wait();
            interpreterFuture.wait();
        });

    // Schedule the lookup of the method implementations within 'dylib', using 'promise' to signal completion.
    // 'setImplementation' is called for every method with the corresponding lookup result.
    auto scheduleLookup = [&](llvm::orc::JITDylib& dylib, std::promise<void>& promise, auto setImplementation)
    {
        m_session->lookup(
            llvm::orc::LookupKind::Static, llvm::orc::makeJITDylibSearchOrder({&dylib}),
            llvm::orc::SymbolLookupSet::fromMapKeys(methodMapping), llvm::orc::SymbolState::Ready,
            [&](llvm::Expected<llvm::orc::SymbolMap> symbolMap)
            {
                if (!symbolMap)
                {
                    llvm::report_fatal_error(symbolMap.takeError());
                }

                for (auto [symbol, result] : *symbolMap)
                {
                    setImplementation(methodMapping.lookup(symbol), result.getAddress());
                }
                promise.set_value();
            },
            llvm::orc::NoDependenciesToRegister);
    };

    scheduleLookup(m_jitCCStubs, jitLookupDone, [](Method* method, llvm::JITTargetAddress targetAddress)
                   { method->setJITCCImplementation(reinterpret_cast<void*>(targetAddress)); });
    scheduleLookup(m_interpreterCCStubs, interpreterLookupDone, [](Method* method, llvm::JITTargetAddress targetAddress)
                   { method->setInterpreterCCImplementation(reinterpret_cast<InterpreterCC*>(targetAddress)); });

    // Interfaces and abstract classes have neither VTables nor ITables to initialize.
    if (classObject.isInterface() || classObject.isAbstract())
    {
        return;
    }

    // Initialize the VTable slots of 'classObject' by initializing them with the methods being executed after method
    // selection.
    for (const ClassObject* curr : classObject.getSuperClasses())
    {
        for (const Method& iter : curr->getMethods())
        {
            auto slot = iter.getTableSlot();
            if (!slot)
            {
                continue;
            }

            const Method& selection = classObject.methodSelection(iter);
            if (selection.isAbstract())
            {
                continue;
            }
            classObject.getVTable()[*slot] = lookupJITCC(selection);
        }
    }

    // Initialize the ITable slots of 'classObject'.
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

            const Method& selection = classObject.methodSelection(iter);
            if (selection.isAbstract())
            {
                continue;
            }
            iTable->getMethods()[*slot] = lookupJITCC(selection);
        }
    }
}

void jllvm::Runtime::doOnStackReplacement(JavaFrame frame, OSRState&& state)
{
    void* entry =
        state.getTarget().getOSREntry(*frame.getMethod(), state.getByteCodeOffset(), frame.getCallingConvention());
    frame.getUnwindFrame().resumeExecutionAtFunction(reinterpret_cast<void (*)(std::uint64_t*)>(entry),
                                                     state.release());
}

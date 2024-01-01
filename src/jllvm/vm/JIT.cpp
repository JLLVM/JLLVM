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

#include <llvm/ExecutionEngine/Orc/Shared/OrcError.h>

#include <jllvm/materialization/InvokeStubsDefinitionsGenerator.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <utility>

#include "VirtualMachine.hpp"

#define DEBUG_TYPE "jvm"

namespace
{

void allowDuplicateDefinitions(llvm::Error&& error)
{
    llvm::handleAllErrors(std::move(error), [](const llvm::orc::DuplicateDefinition&) {});
}

} // namespace

jllvm::JIT::JIT(VirtualMachine& virtualMachine)
    : m_virtualMachine(virtualMachine),
      m_javaJITSymbols(
          llvm::cantFail(virtualMachine.getRuntime().getCLibDylib().getExecutionSession().createJITDylib("<javaJIT>"))),
      m_javaJITImplDetails(
          llvm::cantFail(m_javaJITSymbols.getExecutionSession().createJITDylib("<javaJITImplDetails>"))),
      m_interpreter2JITSymbols(
          llvm::cantFail(m_javaJITSymbols.getExecutionSession().createJITDylib("<interpreter2jit>"))),
      m_byteCodeCompileLayer(virtualMachine.getRuntime().getLLVMIRLayer(), virtualMachine.getRuntime().getInterner(),
                             virtualMachine.getRuntime().getDataLayout()),
      m_byteCodeOSRCompileLayer(m_byteCodeCompileLayer.getBaseLayer(), m_byteCodeCompileLayer.getInterner(),
                                m_byteCodeCompileLayer.getDataLayout())
{
    // JITted Java methods mustn't lookup symbols within 'm_javaJITSymbols', as these are always JITted methods, but
    // rather resolve direct method calls to the stubs in the runtimes JITCC dylib.
    Runtime& runtime = virtualMachine.getRuntime();
    llvm::orc::JITDylibSearchOrder searchOrder = {
        {&runtime.getJITCCDylib(), llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&m_javaJITImplDetails, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&runtime.getClassAndMethodObjectsDylib(), llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&runtime.getCLibDylib(), llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
    };

    m_javaJITSymbols.setLinkOrder(searchOrder, /*LinkAgainstThisJITDylibFirst=*/false);

    // The functions created by the 'InvokeStubsDefinitionsGenerator' are also considered an
    // implementation detail and may only link against the stubs.
    m_javaJITImplDetails.addGenerator(std::make_unique<InvokeStubsDefinitionsGenerator>(
        runtime.createIndirectStubsManager(), runtime.getLLVMIRLayer(), runtime.getDataLayout(), searchOrder,
        m_virtualMachine.getClassLoader()));

    GarbageCollector& gc = m_virtualMachine.getGC();
    ClassLoader& classLoader = m_virtualMachine.getClassLoader();

    runtime.addImplementationSymbols(
        m_javaJITImplDetails, std::pair{"jllvm_gc_alloc", [&](std::uint32_t size) { return gc.allocate(size); }},
        std::pair{"jllvm_for_name_loaded",
                  [&](const char* name) { return classLoader.forNameLoaded(FieldType(name)); }},
        std::pair{"jllvm_instance_of",
                  [](const Object* object, const ClassObject* classObject) -> std::int32_t
                  { return object->instanceOf(classObject); }},
        std::pair{"jllvm_osr_frame_delete", [](const std::uint64_t* osrFrame) { delete[] osrFrame; }},
        std::pair{"jllvm_throw", [&](Throwable* object) { m_virtualMachine.throwJavaException(object); }},
        std::pair{"jllvm_initialize_class_object",
                  [&](ClassObject* classObject)
                  {
                      // This should have been checked inline in LLVM IR.
                      assert(!classObject->isInitialized());
                      m_virtualMachine.initialize(*classObject);
                  }},
        std::pair{"jllvm_throw_class_cast_exception", [&](ObjectInterface* object, ClassObject* classObject)
                  { m_virtualMachine.throwClassCastException(object, classObject); }},
        std::pair{"jllvm_throw_null_pointer_exception", [&]() { m_virtualMachine.throwNullPointerException(); }},
        std::pair{"jllvm_throw_array_index_out_of_bounds_exception", [&](std::int32_t index, std::int32_t size)
                  { m_virtualMachine.throwArrayIndexOutOfBoundsException(index, size); }},
        std::pair{"jllvm_throw_negative_array_size_exception",
                  [&](std::int32_t size) { m_virtualMachine.throwNegativeArraySizeException(size); }});
}

void jllvm::JIT::add(const Method& method)
{
    llvm::cantFail(m_byteCodeCompileLayer.add(m_javaJITSymbols, &method));
    llvm::cantFail(m_virtualMachine.getRuntime().getInterpreter2JITLayer().add(m_interpreter2JITSymbols, &method));
}

void* jllvm::JIT::getOSREntry(const jllvm::Method& method, std::uint16_t byteCodeOffset,
                              CallingConvention callingConvention)
{
    llvm::orc::SymbolStringPtr mangledName =
        m_byteCodeOSRCompileLayer.getInterner()(mangleOSRMethod(&method, byteCodeOffset));
    allowDuplicateDefinitions(
        m_byteCodeOSRCompileLayer.add(m_javaJITSymbols, &method, byteCodeOffset, callingConvention));

    llvm::JITEvaluatedSymbol osrMethod =
        llvm::cantFail(m_virtualMachine.getRuntime().getSession().lookup({&m_javaJITSymbols}, mangledName));
    return reinterpret_cast<void*>(osrMethod.getAddress());
}

jllvm::OSRState jllvm::JIT::createOSRStateFromInterpreterFrame(InterpreterFrame frame)
{
    return OSRState(*this, *frame.getByteCodeOffset(), createOSRBuffer(frame.readLocals(), frame.getOperandStack()));
}

jllvm::OSRState jllvm::JIT::createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                                              Throwable* throwable)
{
    return OSRState(*this, handlerOffset,
                    createOSRBuffer(frame.readLocals(),
                                    /*operandStack=*/std::initializer_list<std::uint64_t>{
                                        reinterpret_cast<std::uint64_t>(throwable)}));
}

std::unique_ptr<std::uint64_t[]> jllvm::JIT::createOSRBuffer(llvm::ArrayRef<std::uint64_t> locals,
                                                             llvm::ArrayRef<std::uint64_t> operandStack)
{
    auto buffer = std::make_unique<std::uint64_t[]>(locals.size() + operandStack.size());

    auto* outIter = llvm::copy(locals, buffer.get());
    llvm::copy(operandStack, outIter);
    return buffer;
}

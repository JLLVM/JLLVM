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

#pragma once

#include <llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/EPCIndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ThreadPool.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/compiler/ClassObjectStubMangling.hpp>
#include <jllvm/gc/GarbageCollector.hpp>
#include <jllvm/materialization/ByteCodeCompileLayer.hpp>
#include <jllvm/materialization/ByteCodeOSRCompileLayer.hpp>
#include <jllvm/materialization/InterpreterOSRLayer.hpp>
#include <jllvm/materialization/JIT2InterpreterLayer.hpp>
#include <jllvm/materialization/JNIImplementationLayer.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>
#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/object/StringInterner.hpp>
#include <jllvm/unwind/Unwinder.hpp>

#include <memory>

#include "JavaFrame.hpp"
#include "OSRState.hpp"
#include "Runtime.hpp"

namespace jllvm
{

class VirtualMachine;

class JIT : public OSRTarget
{
    VirtualMachine& m_virtualMachine;

    llvm::orc::JITDylib& m_javaJITSymbols;
    llvm::orc::JITDylib& m_javaJITImplDetails;

    ByteCodeCompileLayer m_byteCodeCompileLayer;
    ByteCodeOSRCompileLayer m_byteCodeOSRCompileLayer;

    /// Add all symbol-implementation pairs to the implementation library.
    /// The implementation library contains implementation of functions used by the materialization
    /// (bytecode compiler, JNI bridge, etc.).
    template <class Ss, class... Fs>
    void addImplementationSymbols(std::pair<Ss, Fs>&&... args)
    {
        (addImplementationSymbol(std::move(args.first), std::move(args.second)), ...);
    }

    /// Add callable 'f' as implementation for symbol 'symbol' to the implementation library.
    template <class F>
    void addImplementationSymbol(std::string symbol, const F& f)
    {
        llvm::cantFail(m_javaJITImplDetails.define(createLambdaMaterializationUnit(
            std::move(symbol), m_byteCodeCompileLayer.getBaseLayer(), f, m_byteCodeCompileLayer.getDataLayout(),
            m_byteCodeCompileLayer.getInterner())));
    }

    static std::unique_ptr<std::uint64_t[]> createOSRBuffer(llvm::ArrayRef<std::uint64_t> locals,
                                                            llvm::ArrayRef<std::uint64_t> operandStack);

public:
    explicit JIT(VirtualMachine& virtualMachine);

    /// Adds and registers a class in the JIT. This has to be done prior to being able to lookup and execute
    /// any methods defined within the class file.
    void add(const Method& method) override;

    bool canExecute(const Method& method) const override
    {
        return !(method.isNative() || method.isAbstract());
    }

    llvm::orc::JITDylib& getJITCCDylib() override
    {
        return m_javaJITSymbols;
    }

    void* getOSREntry(const Method& method, std::uint16_t byteCodeOffset) override;

    OSRState createOSRStateFromInterpreterFrame(InterpreterFrame frame) override;

    OSRState createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                               Throwable* throwable) override;
};
} // namespace jllvm

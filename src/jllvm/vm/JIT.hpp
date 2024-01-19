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

#include <jllvm/materialization/ByteCodeCompileLayer.hpp>
#include <jllvm/materialization/ByteCodeOSRCompileLayer.hpp>

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
    llvm::orc::JITDylib& m_interpreter2JITSymbols;

    ByteCodeCompileLayer m_byteCodeCompileLayer;
    ByteCodeOSRCompileLayer m_byteCodeOSRCompileLayer;

    static std::unique_ptr<std::uint64_t[]> createOSRBuffer(llvm::ArrayRef<std::uint64_t> locals,
                                                            llvm::ArrayRef<std::uint64_t> operandStack);

public:
    explicit JIT(VirtualMachine& virtualMachine);

    void add(const Method& method) override;

    bool canExecute(const Method& method) const override
    {
        return !(method.isNative() || method.isAbstract());
    }

    llvm::orc::JITDylib& getJITCCDylib() override
    {
        return m_javaJITSymbols;
    }

    llvm::orc::JITDylib& getInterpreterCCDylib() override
    {
        return m_interpreter2JITSymbols;
    }

    void* getOSREntry(const Method& method, std::uint16_t byteCodeOffset, CallingConvention callingConvention) override;

    OSRState createOSRStateFromInterpreterFrame(InterpreterFrame frame) override;

    OSRState createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                               Throwable* throwable) override;
};
} // namespace jllvm

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

#include <jllvm/materialization/JNIImplementationLayer.hpp>
#include <jllvm/materialization/LambdaMaterialization.hpp>

#include "Runtime.hpp"

namespace jllvm
{

/// Executor instance used to execute any Java methods marked as 'native'. It performs the lookup and adaptor
/// generation for the Java Native Interface (JNI).
class JNIBridge : public Executor
{
    VirtualMachine& m_virtualMachine;

    llvm::orc::JITDylib& m_jniSymbols;
    llvm::orc::JITDylib& m_interpreter2JNISymbols;

    JNIImplementationLayer m_jniImplementationLayer;

public:
    explicit JNIBridge(VirtualMachine& virtualMachine, void* jniEnv);

    /// Adds a new materialization unit to the JNI dylib which will be used to lookup any symbols when 'native' methods
    /// are called.
    void addJNISymbols(std::unique_ptr<llvm::orc::MaterializationUnit>&& materializationUnit)
    {
        m_jniImplementationLayer.define(std::move(materializationUnit));
    }

    /// Adds a new function object 'f' implementing the JNI function 'symbol'. This function object will then be called
    /// if any Java code calls the native method corresponding to the JNI mangled name passed in as 'symbol'.
    /// 'f' must be trivially copyable type.
    template <class F>
    void addJNISymbol(std::string symbol, const F& f)
    {
        m_jniImplementationLayer.define(createLambdaMaterializationUnit(
            std::move(symbol), m_jniImplementationLayer.getBaseLayer(), f, m_jniImplementationLayer.getDataLayout(),
            m_jniImplementationLayer.getInterner()));
    }

    void add(const Method& method) override;

    bool canExecute(const Method& method) const override
    {
        return method.isNative();
    }

    llvm::orc::JITDylib& getJITCCDylib() override
    {
        return m_jniSymbols;
    }

    llvm::orc::JITDylib& getInterpreterCCDylib() override
    {
        return m_interpreter2JNISymbols;
    }
};

} // namespace jllvm

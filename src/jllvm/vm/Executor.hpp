
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

#include <llvm/ExecutionEngine/Orc/Core.h>

#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{
/// Abstract interface for all classes capable of executing Java methods.
class Executor
{
public:
    Executor() = default;

    virtual ~Executor() = default;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    /// Registers a method within the executor, making it available in the dylib returned by 'getJITCCDylib'.
    /// This method assumes that 'canExecute' returned true for 'method'.
    virtual void add(const Method& method) = 0;

    /// Returns true if the executor is capable of executing 'method'.
    virtual bool canExecute(const Method& method) const = 0;

    /// Returns the dylib used for lookups when calling a given method with the JIT Calling Convention.
    /// All registered methods must be contained with the "direct-method-call" mangling.
    virtual llvm::orc::JITDylib& getJITCCDylib() = 0;
};

} // namespace jllvm

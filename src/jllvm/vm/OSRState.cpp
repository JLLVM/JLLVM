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

#include "OSRState.hpp"

jllvm::OSRState jllvm::OSRState::fromInterpreter(InterpreterFrame sourceFrame, OSRTarget target)
{
    switch (target)
    {
        case OSRTarget::Interpreter: llvm::report_fatal_error("not yet implemented");
        case OSRTarget::JIT:
            return OSRState(*sourceFrame.getByteCodeOffset(), sourceFrame.readLocals(), sourceFrame.getOperandStack());
    }
}

jllvm::OSRState jllvm::OSRState::fromException(JavaFrame sourceFrame, std::uint16_t handlerOffset, Throwable* exception,
                                               OSRTarget target)
{
    assert(!sourceFrame.isNative() && "cannot OSR out of native frame");

    switch (target)
    {
        case OSRTarget::Interpreter: llvm::report_fatal_error("not yet implemented");
        case OSRTarget::JIT:
            return OSRState(
                handlerOffset, sourceFrame.readLocals(),
                /*operandStack=*/std::initializer_list<std::uint64_t>{reinterpret_cast<std::uint64_t>(exception)});
    }
}

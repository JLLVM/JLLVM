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

#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/MathExtras.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>

#include "JavaFrame.hpp"

namespace jllvm
{

/// Target tier that should replace the existing Java frame.
enum class OSRTarget
{
    Interpreter,
    JIT,
};

/// Class representing the abstract machine state required for transitioning execution from one tier to another.
/// It is used to internally build up a buffer in the OSR calling convention to initialize the abstract machine state.
class OSRState
{
    std::unique_ptr<std::uint64_t[]> m_buffer;
    OSRTarget m_target{};
    std::uint16_t m_byteCodeOffset{};

    static std::size_t getNumGCMask(std::uint16_t size)
    {
        return llvm::divideCeil(size, 64);
    }

    static std::size_t calculateInterpreterBufferSize(std::uint16_t numLocalVariables, std::uint16_t numOperandStack)
    {
        return 1 + numLocalVariables + numOperandStack + getNumGCMask(numLocalVariables)
               + getNumGCMask(numOperandStack);
    }

    static std::size_t calculateJITBufferSize(std::uint16_t numLocalVariables, std::uint16_t numOperandStack)
    {
        return numLocalVariables + numOperandStack;
    }

    OSRState(std::uint16_t byteCodeOffset, auto&& locals, auto&& operandStack, auto&& localsGCMask,
             auto&& operandStackGCMask)
        : m_target(OSRTarget::Interpreter), m_byteCodeOffset(byteCodeOffset)
    {
        std::size_t numLocals = llvm::size(locals);
        std::size_t numOperandStack = llvm::size(operandStack);

        m_buffer = std::make_unique<std::uint64_t[]>(calculateInterpreterBufferSize(numLocals, numOperandStack));
        m_buffer[0] = byteCodeOffset | numOperandStack << 16;

        auto outIter = llvm::copy(std::forward<decltype(locals)>(locals), std::next(m_buffer.get()));
        outIter = llvm::copy(std::forward<decltype(operandStack)>(operandStack), outIter);
        outIter = std::copy_n(std::begin(std::forward<decltype(localsGCMask)>(localsGCMask)), getNumGCMask(numLocals),
                              outIter);
        std::copy_n(std::begin(std::forward<decltype(operandStackGCMask)>(operandStackGCMask)),
                    getNumGCMask(numOperandStack), outIter);
    }

    OSRState(std::uint16_t byteCodeOffset, auto&& locals, auto&& operandStack)
        : m_target(OSRTarget::JIT), m_byteCodeOffset(byteCodeOffset)
    {
        std::size_t numLocals = llvm::size(locals);
        std::size_t numOperandStack = llvm::size(operandStack);

        m_buffer = std::make_unique<std::uint64_t[]>(calculateJITBufferSize(numLocals, numOperandStack));

        auto outIter = llvm::copy(std::forward<decltype(locals)>(locals), m_buffer.get());
        outIter = llvm::copy(std::forward<decltype(operandStack)>(operandStack), outIter);
    }

public:
    /// Creates an OSRState from an interpreter frame.
    static OSRState fromInterpreter(InterpreterFrame sourceFrame, OSRTarget target);

    /// Creates an OSRState for exception handling from a source frame and an exception.
    static OSRState fromException(JavaFrame sourceFrame, std::uint16_t handlerOffset, Throwable* exception,
                                  OSRTarget target);

    /// Releases the internal buffer filled with the OSR state and returns it.
    ///
    /// The pointed to array depends on the target being OSRed into.
    ///
    /// If the target is an interpreter frame, the memory has the following layout:
    /// std::uint64_t firstElement = byteCodeOffset | numOperandStack << 16
    /// std::uint64_t localVariables[numLocalVariables]
    /// std::uint64_t operandStack[numOperandStack]
    /// std::uint64_t localVariablesGCMask[ceil(numLocalVariables/64)]
    /// std::uint64_t operandStackGCMask[ceil(numOperandStack/64)]
    ///
    /// If the target is a JIT frame, the memory has the following layout:
    ///  std::uint64_t localVariables[numLocalVariables]
    ///  std::uint64_t operandStack[numOperandStack]
    ///
    /// This array is used by OSR versions to initialize their machine state.
    std::uint64_t* release()
    {
        assert(m_buffer && "must not have been released previously");
        return m_buffer.release();
    }

    /// Returns the bytecode offset with which this instance was initialized.
    std::uint16_t getByteCodeOffset() const
    {
        return m_byteCodeOffset;
    }

    /// Returns the OSR target of this state.
    OSRTarget getTarget() const
    {
        return m_target;
    }
};
} // namespace jllvm

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

#include <llvm/Support/Casting.h>

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/unwind/Unwinder.hpp>

namespace jllvm
{

/// Class representing a specialization of 'UnwindFrame' for frames executing Java methods.
/// This class allows accessing properties that are specific to a Java frame, such as the currently executing method
/// or bytecode offset.
class JavaFrame
{
protected:
    const JavaMethodMetadata* m_javaMethodMetadata;
    UnwindFrame* m_unwindFrame;

public:
    /// Constructs a 'JavaFrame' from a frame and its corresponding java method metadata.
    explicit JavaFrame(const JavaMethodMetadata& javaMethodMetadata, UnwindFrame& frame)
        : m_javaMethodMetadata(&javaMethodMetadata), m_unwindFrame(&frame)
    {
    }

    /// Returns true if this java frame is being executed in the JIT.
    bool isJIT() const
    {
        return m_javaMethodMetadata->isJIT();
    }

    /// Returns true if this java frame is being executed in the interpreter.
    bool isInterpreter() const
    {
        return m_javaMethodMetadata->isInterpreter();
    }

    /// Returns true if this java frame is a native method.
    bool isNative() const
    {
        return m_javaMethodMetadata->isNative();
    }

    /// Returns the bytecode offset of the frame currently being executed.
    /// Returns an empty optional if the method being executed is native and therefore does not have a bytecode offset.
    std::optional<std::uint16_t> getByteCodeOffset() const;

    /// Returns the method object currently being executed.
    const Method* getMethod() const
    {
        return m_javaMethodMetadata->getMethod();
    }

    /// Returns the enclosing class object of the method currently being executed.
    const ClassObject* getClassObject() const
    {
        return getMethod()->getClassObject();
    }

    /// Returns the lower level unwind frame of the Java frame.
    UnwindFrame& getUnwindFrame() const
    {
        return *m_unwindFrame;
    }

    /// Returns the lower level java method metadata of the Java frame.
    const JavaMethodMetadata& getJavaMethodMetadata() const
    {
        return *m_javaMethodMetadata;
    }

    /// Reads out the values of all the local variables at the current bytecode offset.
    /// This method will always return an empty array in following scenarios:
    /// * If the method being executed is native and therefore does not have local variables
    /// * If no exception handler exists for a bytecode offset within a JITted method.
    llvm::SmallVector<std::uint64_t> readLocals() const;

    /// Reads out the values of the operand stack at the current bytecode offset.
    /// This method will always return an empty array in following scenarios:
    /// * If the method being executed is not being executed by the interpreter.
    llvm::ArrayRef<std::uint64_t> readOperandStack() const;
};

/// Specialization of 'JavaFrame' for interpreter frames. This contains all methods specific to interpreter frames.
/// LLVMs casting infrastructure can be used to cast between 'InterpreterFrame' and 'JavaFrame'.
/// E.g.
/// if (llvm::isa<InterpreterFrame>(javaFrame)) { ... }
/// std::optional<InterpreterFrame> interpreterFrame = llvm::dyn_cast<InterpreterFrame>(javaFrame);
/// InterpreterFrame interpreterFrame = llvm::cast<InterpreterFrame>(javaFrame);
class InterpreterFrame : public JavaFrame
{
    using JavaFrame::JavaFrame;

    template <typename To, typename From, typename Enable>
    friend struct llvm::CastInfo;

public:
    /// Returns a mutable view of the locals of the interpreter.
    llvm::MutableArrayRef<std::uint64_t> getLocals() const;

    /// Returns the bitset denoting where Java references are contained within the interpreter locals.
    llvm::ArrayRef<std::uint64_t> getLocalsGCMask() const;

    /// Returns a mutable view of the operand stack of the interpreter.
    llvm::MutableArrayRef<std::uint64_t> getOperandStack() const;

    /// Returns the bitset denoting where Java references are contained within the interpreter operand stack.
    llvm::ArrayRef<std::uint64_t> getOperandStackGCMask() const;
};

} // namespace jllvm

/// Hooks casting 'JavaFrame' to 'InterpreterFrame' into LLVMs casting infrastructure.
template <>
struct llvm::CastInfo<jllvm::InterpreterFrame, jllvm::JavaFrame>
    : llvm::DefaultDoCastIfPossible<std::optional<jllvm::InterpreterFrame>, jllvm::JavaFrame,
                                    llvm::CastInfo<jllvm::InterpreterFrame, jllvm::JavaFrame>>
{
    static bool isPossible(jllvm::JavaFrame frame)
    {
        return frame.isInterpreter();
    }

    static jllvm::InterpreterFrame doCast(jllvm::JavaFrame frame)
    {
        return jllvm::InterpreterFrame(frame.getJavaMethodMetadata(), frame.getUnwindFrame());
    }

    static std::optional<jllvm::InterpreterFrame> castFailed()
    {
        return std::nullopt;
    }
};

/// Specialization for when 'JavaFrame' is const to dispatch to the non-const specialization.
template <>
struct llvm::CastInfo<jllvm::InterpreterFrame, const jllvm::JavaFrame>
    : llvm::CastInfo<jllvm::InterpreterFrame, jllvm::JavaFrame>
{
};

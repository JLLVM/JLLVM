
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <jllvm/compiler/ByteCodeCompileUtils.hpp>
#include <jllvm/unwind/Unwinder.hpp>

namespace jllvm
{

/// Class representing a specialization of 'UnwindFrame' for frames executing Java methods.
/// This class allows accessing properties that are specific to a Java frame, such as the currently executing method
/// or bytecode offset.
class JavaFrame
{
    const JavaMethodMetadata* m_javaMethodMetadata;
    UnwindFrame* m_unwindFrame;

public:
    /// Constructs a 'JavaFrame' from a frame and its corresponding java method metadata.
    explicit JavaFrame(const JavaMethodMetadata& javaMethodMetadata, UnwindFrame& frame)
        : m_javaMethodMetadata(&javaMethodMetadata), m_unwindFrame(&frame)
    {
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

    /// Reads out the values of all the local variables at the given bytecode offset.
    /// This method will always return an empty array in following scenarios:
    /// * If the method being executed is native and therefore does not have local variables
    /// * If no exception handler exists for a bytecode offset within a JITted method.
    llvm::SmallVector<std::uint64_t> readLocals() const;
};

} // namespace jllvm

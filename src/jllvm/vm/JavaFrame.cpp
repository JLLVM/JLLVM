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

#include "JavaFrame.hpp"

std::optional<std::uint16_t> jllvm::JavaFrame::getByteCodeOffset() const
{
    switch (m_javaMethodMetadata->getKind())
    {
        case JavaMethodMetadata::Kind::JIT:
            return m_javaMethodMetadata->getJITData()[m_unwindFrame->getProgramCounter()].byteCodeOffset;
        case JavaMethodMetadata::Kind::Interpreter:
            return *m_javaMethodMetadata->getInterpreterData().byteCodeOffset.readScalar(*m_unwindFrame);
        case JavaMethodMetadata::Kind::Native: return std::nullopt;
    }
    llvm_unreachable("invalid kind");
}

llvm::SmallVector<std::uint64_t> jllvm::JavaFrame::readLocals() const
{
    switch (m_javaMethodMetadata->getKind())
    {
        case JavaMethodMetadata::Kind::Native: return {};
        case JavaMethodMetadata::Kind::JIT:
        {
            llvm::ArrayRef<FrameValue<std::uint64_t>> locals =
                m_javaMethodMetadata->getJITData()[m_unwindFrame->getProgramCounter()].locals;
            return llvm::to_vector(llvm::map_range(locals, [&](FrameValue<std::uint64_t> frameValue)
                                                   { return frameValue.readScalar(*m_unwindFrame); }));
        }
        case JavaMethodMetadata::Kind::Interpreter:
            return llvm::to_vector(llvm::cast<InterpreterFrame>(*this).getLocals());
    }
    llvm_unreachable("invalid kind");
}

llvm::MutableArrayRef<std::uint64_t> jllvm::InterpreterFrame::getLocals() const
{
    std::uint16_t numLocals =
        m_javaMethodMetadata->getMethod()->getMethodInfo().getAttributes().find<Code>()->getMaxLocals();
    std::uint64_t* locals = m_javaMethodMetadata->getInterpreterData().localVariables.readScalar(*m_unwindFrame);
    return llvm::MutableArrayRef<std::uint64_t>(locals, locals + numLocals);
}

jllvm::BitArrayRef<> jllvm::InterpreterFrame::getLocalsGCMask() const
{
    std::uint16_t numLocals =
        m_javaMethodMetadata->getMethod()->getMethodInfo().getAttributes().find<Code>()->getMaxLocals();
    std::uint64_t* mask = m_javaMethodMetadata->getInterpreterData().localVariablesGCMask.readScalar(*m_unwindFrame);
    return BitArrayRef<>(mask, numLocals);
}

llvm::MutableArrayRef<std::uint64_t> jllvm::InterpreterFrame::getOperandStack() const
{
    std::uint16_t numStack = *m_javaMethodMetadata->getInterpreterData().topOfStack.readScalar(*m_unwindFrame);
    std::uint64_t* operands = m_javaMethodMetadata->getInterpreterData().operandStack.readScalar(*m_unwindFrame);
    return llvm::MutableArrayRef<std::uint64_t>(operands, operands + numStack);
}

jllvm::BitArrayRef<> jllvm::InterpreterFrame::getOperandStackGCMask() const
{
    std::uint16_t numStack = *m_javaMethodMetadata->getInterpreterData().topOfStack.readScalar(*m_unwindFrame);
    std::uint64_t* mask = m_javaMethodMetadata->getInterpreterData().operandGCMask.readScalar(*m_unwindFrame);
    return BitArrayRef<>(mask, numStack);
}

llvm::SmallVector<std::uint64_t> jllvm::JavaFrame::readLocalsGCMask() const
{
    switch (m_javaMethodMetadata->getKind())
    {
        case JavaMethodMetadata::Kind::Native: return {};
        case JavaMethodMetadata::Kind::JIT:
            return llvm::to_vector(m_javaMethodMetadata->getJITData()[m_unwindFrame->getProgramCounter()].localsGCMask);

        case JavaMethodMetadata::Kind::Interpreter:
        {
            BitArrayRef<> bitArray = llvm::cast<InterpreterFrame>(*this).getLocalsGCMask();
            return llvm::SmallVector<std::uint64_t>(bitArray.words_begin(), bitArray.words_end());
        }
    }
}

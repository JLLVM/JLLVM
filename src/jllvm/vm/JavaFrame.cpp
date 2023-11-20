// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "JavaFrame.hpp"

std::optional<std::uint16_t> jllvm::JavaFrame::getByteCodeOffset() const
{
    switch (m_javaMethodMetadata->getKind())
    {
        case JavaMethodMetadata::Kind::JIT:
            return m_javaMethodMetadata->getJITData()[m_unwindFrame->getProgramCounter()].byteCodeOffset;
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
    }
    llvm_unreachable("invalid kind");
}

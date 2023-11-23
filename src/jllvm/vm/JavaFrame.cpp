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

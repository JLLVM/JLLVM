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

#include "Unwinder.hpp"

#include <llvm/Support/Debug.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

#include <utility>

#include <jllvm_unwind.h>

#define DEBUG_TYPE "unwinder"

bool jllvm::detail::unwindInternal(void* lambdaIn, UnwindAction (*fpIn)(void*, UnwindFrame))
{
    std::pair data{lambdaIn, fpIn};
    jllvm_Unwind_Reason_Code code = jllvm_Unwind_Backtrace(
        +[](jllvm_Unwind_Context* context, void* lp)
        {
            auto [lambda, fp] = *reinterpret_cast<decltype(data)*>(lp);
            switch (fp(lambda, UnwindFrame(context)))
            {
                case UnwindAction::ContinueUnwinding: return jllvm_URC_NO_REASON;
                case UnwindAction::StopUnwinding: return jllvm_URC_END_OF_STACK;
                default: llvm_unreachable("Invalid unwind action");
            }
        },
        &data);
    return code != jllvm_URC_END_OF_STACK;
}

std::uintptr_t jllvm::UnwindFrame::getProgramCounter() const
{
    return jllvm_Unwind_GetIP(reinterpret_cast<jllvm_Unwind_Context*>(m_impl));
}

std::uintptr_t jllvm::UnwindFrame::getIntegerRegister(int registerNumber) const
{
    return jllvm_Unwind_GetGR(reinterpret_cast<jllvm_Unwind_Context*>(m_impl), registerNumber);
}

void jllvm::UnwindFrame::setIntegerRegister(int registerNumber, std::uintptr_t value)
{
    jllvm_Unwind_SetGR(reinterpret_cast<jllvm_Unwind_Context*>(m_impl), registerNumber, value);
}

std::uintptr_t jllvm::UnwindFrame::getFunctionPointer() const
{
    return jllvm_Unwind_GetRegionStart(reinterpret_cast<jllvm_Unwind_Context*>(m_impl));
}

namespace
{

/// Walks through a 'eh_frame', finding all DWARF FDEs to register them in the unwinder.
/// Taken from
/// https://github.com/llvm/llvm-project/blob/aa5158cd1ee01625fbbe6fb106b0f2598b0fdf72/llvm/lib/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.cpp#L91
template <typename HandleFDEFn>
void walkLibunwindEHFrameSection(const char* const sectionStart, std::size_t sectionSize, HandleFDEFn handleFDE)
{
    const char* curCfiRecord = sectionStart;
    const char* end = sectionStart + sectionSize;
    std::uint64_t size = *reinterpret_cast<const uint32_t*>(curCfiRecord);

    while (curCfiRecord != end && size != 0)
    {
        const char* offsetField = curCfiRecord + (size == 0xffffffff ? 12 : 4);
        if (size == 0xffffffff)
        {
            size = *reinterpret_cast<const std::uint64_t*>(curCfiRecord + 4) + 12;
        }
        else
        {
            size += 4;
        }
        auto offset = *reinterpret_cast<const std::uint32_t*>(offsetField);

        LLVM_DEBUG({
            llvm::dbgs() << "Registering eh-frame section:\n";
            llvm::dbgs() << "Processing " << (offset ? "FDE" : "CIE") << " @" << (void*)curCfiRecord << ": [";
            for (unsigned I = 0; I < size; ++I)
            {
                llvm::dbgs() << llvm::format(" 0x%02" PRIx8, *(curCfiRecord + I));
            }
            llvm::dbgs() << " ]\n";
        });

        if (offset != 0)
        {
            handleFDE(curCfiRecord);
        }

        curCfiRecord += size;

        size = *reinterpret_cast<const uint32_t*>(curCfiRecord);
    }
}

} // namespace

void jllvm::registerEHSection(llvm::ArrayRef<char> section)
{
    walkLibunwindEHFrameSection(section.data(), section.size(), [](const char* fde) { jllvm__register_frame(fde); });
}

void jllvm::deregisterEHSection(llvm::ArrayRef<char> section)
{
    walkLibunwindEHFrameSection(section.data(), section.size(), [](const char* fde) { jllvm__deregister_frame(fde); });
}

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

namespace
{
///
void cantFail([[maybe_unused]] int unwindErrorCode)
{
    assert(unwindErrorCode == 0 && "unwinding action cannot fail");
}
} // namespace

std::uintptr_t jllvm::UnwindFrame::getIntegerRegister(int registerNumber) const
{
    unw_word_t value;
    cantFail(jllvm_unw_get_reg(const_cast<jllvm_unw_cursor_t*>(&m_cursor), registerNumber, &value));
    return value;
}

void jllvm::UnwindFrame::setIntegerRegister(int registerNumber, std::uintptr_t value)
{
    cantFail(jllvm_unw_set_reg(&m_cursor, registerNumber, value));
}

std::uintptr_t jllvm::UnwindFrame::getFunctionPointer() const
{
    jllvm_unw_proc_info_t procInfo;
    cantFail(jllvm_unw_get_proc_info(const_cast<jllvm_unw_cursor_t*>(&m_cursor), &procInfo));
    return procInfo.start_ip;
}

jllvm::UnwindFrame::UnwindFrame(const jllvm_unw_context_t& context) : m_cursor()
{
    cantFail(jllvm_unw_init_local(&m_cursor, const_cast<jllvm_unw_context_t*>(&context)));
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

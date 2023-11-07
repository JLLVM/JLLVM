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
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

#include <jllvm_unwind.h>
#include <unwind.h>

#define DEBUG_TYPE "unwinder"

namespace
{
/// Assert that any use of libunwind didn't cause any errors. Errors while using libunwind are considered toolchain
/// bugs or errors, not expected error cases.
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

void jllvm::UnwindFrame::resumeExecutionAtFunctionImpl(std::uintptr_t functionPointer,
                                                       llvm::ArrayRef<std::uint64_t> arguments) const
{
#if defined(__x86_64__) && !defined(_WIN32)
    constexpr bool stackGrowsDown = true;
    constexpr bool returnAddressOnStack = true;
    constexpr std::array<int, argMax> argRegisterNumbers = {UNW_X86_64_RDI, UNW_X86_64_RSI, UNW_X86_64_RDX,
                                                            UNW_X86_64_RCX, UNW_X86_64_R8,  UNW_X86_64_R9};

#else
    #error Code not ported for this architecture yet
#endif

    assert(arguments.size() <= argRegisterNumbers.size());

    // Get the caller frame of this frame and set registers in that frame. By going to the caller frame, its
    // callee-saved registers are restored to the values right before the call.
    std::optional<UnwindFrame> maybeCallerFrame = this->callerFrame();
    assert(maybeCallerFrame && "Replacing bottom of stack is not supported");
    UnwindFrame& nextFrame = *maybeCallerFrame;

    // The stack pointer value of the caller is right before the call. If the platform also pushes a return address on
    // the stack, adjust the stack pointer past the return address as it would be on functon entry.
    std::uintptr_t nextStack = nextFrame.getIntegerRegister(UNW_REG_SP);
    if constexpr (returnAddressOnStack)
    {
        nextStack += (stackGrowsDown ? -1 : 1) * sizeof(void (*)());
    }

    nextFrame.setIntegerRegister(UNW_REG_IP, functionPointer);
    nextFrame.setIntegerRegister(UNW_REG_SP, nextStack);

    // Set the function arguments.
    for (std::size_t i = 0; i < arguments.size(); i++)
    {
        nextFrame.setIntegerRegister(argRegisterNumbers[i], arguments[i]);
    }

    // Exception object for the force unwind of the C++ stack.
    struct ForcedException : _Unwind_Exception
    {
        UnwindFrame frame;

        explicit ForcedException(const UnwindFrame& frame) : _Unwind_Exception{0}, frame(frame)
        {
            // Identifier used by personality functions to identify the kind of exception object being thrown.
            // Mustn't match what the C++ runtime uses.
            std::memcpy(&exception_class, "JLVMJAVA", sizeof(exception_class));
            exception_cleanup = +[](_Unwind_Reason_Code, _Unwind_Exception* exception)
            { delete static_cast<ForcedException*>(exception); };
        }
    };

    // Exception object for the force unwind must be heap allocated as the stack unwinding destroys all local variables.
    auto* exception = new ForcedException(nextFrame);
    // Unwind the C++ stack until the Java frame that should be replaced is reached. The program counter of that frame
    // is passed as 'stopPc' and always compared with the current frame being unwound by the '_Unwind_ForcedUnwind'
    // implementation.
    _Unwind_ForcedUnwind(
        exception,
        +[](int, _Unwind_Action, std::uint64_t, _Unwind_Exception* exception, _Unwind_Context* context, void* stopPc)
        {
            std::uintptr_t pc = _Unwind_GetIP(context);
            if (pc != reinterpret_cast<std::uintptr_t>(stopPc))
            {
                // Continue unwinding.
                return _URC_NO_REASON;
            }

            // Reached the Java frame to replace. Get the internal cursor that all modifications have been performed on
            // so far and apply them.
            auto* forcedException = static_cast<ForcedException*>(exception);
            jllvm_unw_cursor_t cursor = forcedException->frame.m_cursor;
            // Make sure to now erase the heap allocated exception object.
            _Unwind_DeleteException(exception);
            jllvm_unw_resume(&cursor);

            llvm_unreachable("resume should not have returned");
        },
        reinterpret_cast<void*>(getProgramCounter()));

    llvm_unreachable("_Unwind_ForcedUnwind should not have returned");
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

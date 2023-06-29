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

#include <llvm/Support/ErrorHandling.h>

#include <utility>

#include <unwind.h>

bool jllvm::detail::unwindInternal(void* lambdaIn, UnwindAction (*fpIn)(void*, UnwindFrame))
{
    std::pair data{lambdaIn, fpIn};
    _Unwind_Reason_Code code = _Unwind_Backtrace(
        +[](_Unwind_Context* context, void* lp)
        {
            auto [lambda, fp] = *reinterpret_cast<decltype(data)*>(lp);
            switch (fp(lambda, UnwindFrame(context)))
            {
                case UnwindAction::ContinueUnwinding: return _URC_NO_REASON;
                case UnwindAction::StopUnwinding: return _URC_END_OF_STACK;
                default: llvm_unreachable("Invalid unwind action");
            }
        },
        &data);
    return code != _URC_END_OF_STACK;
}

std::uintptr_t jllvm::UnwindFrame::getProgramCounter() const
{
    return _Unwind_GetIP(reinterpret_cast<_Unwind_Context*>(m_impl));
}

std::uintptr_t jllvm::UnwindFrame::getIntegerRegister(int registerNumber) const
{
    return _Unwind_GetGR(reinterpret_cast<_Unwind_Context*>(m_impl), registerNumber);
}

void jllvm::UnwindFrame::setIntegerRegister(int registerNumber, std::uintptr_t value)
{
    _Unwind_SetGR(reinterpret_cast<_Unwind_Context*>(m_impl), registerNumber, value);
}

std::uintptr_t jllvm::UnwindFrame::getStackPointer() const
{
    return _Unwind_GetCFA(reinterpret_cast<_Unwind_Context*>(m_impl));
}

std::uintptr_t jllvm::UnwindFrame::getFunctionPointer() const
{
    return _Unwind_GetRegionStart(reinterpret_cast<_Unwind_Context*>(m_impl));
}

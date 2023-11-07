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

#include <llvm/ADT/ArrayRef.h>

#include <concepts>
#include <cstdint>
#include <type_traits>

#include <jllvm_libunwind.h>

namespace jllvm
{

/// Class representing a single frame on the stack while unwinding. A frame conceptually consists of the program
/// counter pointing to a call, the stack pointer right before the call, and all callee-saved registers. Caller-saved
/// register cannot arbitrarily be recovered.
class UnwindFrame
{
    jllvm_unw_cursor_t m_cursor;

    explicit UnwindFrame(const jllvm_unw_cursor_t& cursor) : m_cursor(cursor) {}

    // This should be restricted with 'std::invocable<UnwindFrame&>' but isn't to workaround
    // https://github.com/llvm/llvm-project/issues/71595
    template <class F>
    friend bool unwindStack(F&& f);

    UnwindFrame(const jllvm_unw_context_t& context);

#if defined(__x86_64__) && !defined(_WIN32)

    constexpr static std::size_t argMax = 6;

    template <class T>
    constexpr static bool abiSupported = std::is_integral_v<T> || std::is_pointer_v<T>;

#else
    #error Code not ported for this architecture yet
#endif

    [[noreturn]] void resumeExecutionAtFunctionImpl(std::uintptr_t functionPointer,
                                                    llvm::ArrayRef<std::uint64_t> arguments) const;

public:

    /// Returns the current program counter in this frame.
    std::uintptr_t getProgramCounter() const
    {
        return getIntegerRegister(UNW_REG_IP);
    }

    /// Returns the value of the integer register with the given DWARF register number in the current frame, at the
    /// current program counter.
    /// This is only guaranteed to work with callee-saved registers.
    std::uintptr_t getIntegerRegister(int registerNumber) const;

    /// Sets the value of the integer register with the given DWARF register number in the current frame, at the
    /// current program counter.
    /// This is only guaranteed to work with callee-saved registers.
    void setIntegerRegister(int registerNumber, std::uintptr_t value);

    /// Returns the address of the function being executed in this frame.
    std::uintptr_t getFunctionPointer() const;

    /// Returns the frame of caller of this frame, or an emtpy optional if the bottom of the call stack was reached.
    std::optional<UnwindFrame> callerFrame() const
    {
        jllvm_unw_cursor_t copy = m_cursor;
        int result = jllvm_unw_step(&copy);
        if (result == 0)
        {
            // Bottom of the stack.
            return std::nullopt;
        }

        assert(result >= 0 && "expected no errors in libunwind");
        return UnwindFrame(copy);
    }

    /// Replaces this frame and all its direct or indirect callees with the execution of 'fnPtr' called with 'args'.
    /// This first performs C++ stack unwinding to run any destructors in all callee frames. 'fnPtr' is required to have
    /// the same return type or a compatible return type as determined by the platform ABI as the function being
    /// executed by this frame.
    /// The argument types and count supported by 'args' is platform dependent but must currently support at least two
    /// arguments of pointer types.
    template <class Ret, typename... Args>
    [[noreturn]] void resumeExecutionAtFunction(Ret (*fnPtr)(Args...),
                                                typename std::type_identity<Args>::type... args) const
        requires((std::is_void_v<Ret> || abiSupported<Ret>)&&...&& abiSupported<Args>)
    {
        std::array<std::uint64_t, sizeof...(args)> array{llvm::bit_cast<std::uint64_t>(args)...};
        resumeExecutionAtFunctionImpl(reinterpret_cast<std::uintptr_t>(fnPtr), array);
    }
};

/// Optional return type of function object passed to 'unwindStack'.
enum class UnwindAction
{
    /// Causes 'unwindStack' to move onto the next frame on the stack.
    ContinueUnwinding,
    /// Causes 'unwindStack' to stop unwinding and return.
    StopUnwinding
};

/// Function to unwind the stack. 'f' is called with an instance of 'UnwindFrame' for every frame on the stack and
/// may be any callable object. Note that integer registers changes on the frame passed as parameter are currently
/// discarded and not applied.
/// 'f' may return instances of 'UnwindAction' to control the unwinding process. Otherwise, the stack is fully unwound.
/// Returns true if stack unwinding was interrupted.
template <class F>
bool unwindStack(F&& f)
{
    // Note that it is required for this function to be called here. Specifically, the frame calling
    // 'jllvm_unw_getcontext' must remain on the call stack to initialize an 'UnwindFrame' instance from the context.
    jllvm_unw_context_t context;
    jllvm_unw_getcontext(&context);

    using T = decltype(f(std::declval<UnwindFrame&>()));
    for (std::optional<UnwindFrame> frame = UnwindFrame(context); frame; frame = frame->callerFrame())
    {
        if constexpr (std::is_void_v<T>)
        {
            f(*frame);
        }
        else
        {
            if (f(*frame) == UnwindAction::StopUnwinding)
            {
                return true;
            }
        }
    }
    return false;
}

/// Registers a dynamically generated 'eh_section' in the unwinder, making it capable of unwinding through it. This is
/// only required for JIT compiled sections, not any code statically part of the executable.
void registerEHSection(llvm::ArrayRef<char> section);

/// Deregisters a dynamically generated 'eh_section' previously generated with 'registerEHSection'. This deallocates any
/// data and caches associated with that section and should be used prior to deallocating the memory of that section.
void deregisterEHSection(llvm::ArrayRef<char> section);
} // namespace jllvm

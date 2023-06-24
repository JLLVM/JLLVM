#pragma once

#include <cstdint>
#include <type_traits>
#include <concepts>

namespace jllvm
{

/// Optional return type of function object passed to 'unwindStack'.
enum class UnwindAction
{
    /// Causes 'unwindStack' to move onto the next frame on the stack.
    ContinueUnwinding,
    /// Causes 'unwindStack' to stop unwinding and return.
    StopUnwinding
};

class UnwindFrame;

namespace detail
{

bool unwindInternal(void* lambdaIn, UnwindAction (*fpIn)(void*, UnwindFrame));

} // namespace detail

/// Class representing a single frame on the stack while unwinding.
class UnwindFrame
{
    void* m_impl;

    explicit UnwindFrame(void* impl) : m_impl{impl} {}

    friend bool detail::unwindInternal(void* lambdaIn, UnwindAction (*fpIn)(void*, UnwindFrame));

public:

    /// Returns the current program counter in this frame.
    std::uintptr_t getProgramCounter() const;

    /// Returns the value of the integer register with the given DWARF register number in the current frame, at the
    /// current program counter.
    /// This is only guaranteed to work with callee-saved registers.
    std::uintptr_t getIntegerRegister(int registerNumber) const;

    /// Sets the value of the integer register with the given DWARF register number in the current frame, at the
    /// current program counter.
    /// This is only guaranteed to work with callee-saved registers.
    void setIntegerRegister(int registerNumber, std::uintptr_t value);

    /// Returns the value of the stack pointer in the current frame, at the current program counter.
    std::uintptr_t getStackPointer() const;

    /// Returns a pointer to the function being executed in this frame.
    std::uintptr_t getFunctionPointer() const;
};

/// Function to unwind the stack. 'f' is called with an instance of 'UnwindFrame' for every frame on the stack and
/// may be any callable object.
/// 'f' may return instances of 'UnwindAction' to control the unwinding process. Otherwise, the stack is fully unwound.
/// Returns true if stack unwinding was interrupted.
template <std::invocable<UnwindFrame> F>
bool unwindStack(F&& f)
{
    return detail::unwindInternal(
        &f,
        +[](void* lambda, UnwindFrame context)
        {
            using T = decltype((*reinterpret_cast<F*>(lambda))(context));
            if constexpr (std::is_void_v<T>)
            {
                (*reinterpret_cast<F*>(lambda))(context);
                return UnwindAction::ContinueUnwinding;
            }
            else
            {
                return (*reinterpret_cast<F*>(lambda))(context);
            }
        });
}
} // namespace jllvm

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

#include <jllvm/support/Bytes.hpp>

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

    template <std::invocable<UnwindFrame&> F>
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

/// Class representing a specific location of a value within a 'UnwindFrame'.
/// A 'FrameValue' is only ever valid for a specific program counter within frames of a specific function.
/// Mapping the program counter and/or function pointer to frame values is performed by the stackmap.
/// A 'FrameValue' itself does not have any mechanism to validate reading from the correct frame.
/// 'T' can be used to make a stronger typed 'FrameValue' which is known to contain a value of type 'T'.
/// Note that this type is trivially copyable, a standard layout type and small enough to be passed by value rather
/// than const reference.
template <class T = void>
class FrameValue
{
    enum class Tag : std::uint8_t
    {
        /// Value was constant folded.
        Constant = 0,
        /// Value is within a callee-saved register.
        Register = 1,
        /// Value is the result of an 'alloca' instruction.
        Direct = 2,
        /// Value is spilled on the stack.
        Indirect = 3,
    };

    /// Type used to safely access the tag regardless of active union member.
    /// This is legal due to the common initial sequence rule:
    /// https://en.cppreference.com/w/cpp/language/data_members#Standard_layout.
    /// Using an explicit tag inside of the union is beneficial as it allows reducing padding inbetween the tag and
    /// fields after. Using a tagged union rather than a variant so that 'FrameValue' is a standard layout type that
    /// can safely be interoperated with form LLVM.
    struct TagOnly
    {
        Tag tag;
    };

    struct Constant
    {
        Tag tag;
        std::uint64_t constant;
    };

    struct Register
    {
        Tag tag;
        /// Dwarf register number of the register containing the value.
        int registerNumber;
    };

    struct Direct
    {
        Tag tag;
        /// Dwarf register number of the register containing the frame pointer.
        int registerNumber;
        std::int32_t offset;
    };

    struct Indirect
    {
        Tag tag;
        std::uint8_t size;
        /// Dwarf register number of the register containing the frame pointer.
        int registerNumber;
        std::int32_t offset;
    };

    union
    {
        // The first union member is used when default initializing. This initializes it to a constant with 0 as value.
        Constant constant{};
        TagOnly accessTag;
        Register inRegister;
        Direct direct;
        Indirect indirect;
    } m_union{};

public:
    FrameValue() = default;

    /// Creates a 'FrameValue' with a constant value.
    static FrameValue constant(std::uint64_t constant)
    {
        FrameValue result;
        result.m_union.constant = {Tag::Constant, constant};
        return result;
    }

    /// Creates a 'FrameValue' that is within a CSR register.
    static FrameValue inRegister(int registerNumber)
    {
        FrameValue result;
        result.m_union.inRegister = {Tag::Register, registerNumber};
        return result;
    }

    /// Creates a 'FrameValue' that is a stack allocation.
    static FrameValue direct(int registerNumber, std::int32_t offset)
    {
        FrameValue result;
        result.m_union.direct = {Tag::Direct, registerNumber, offset};
        return result;
    }

    /// Creates a 'FrameValue' that has been spilled into a location on the stack.
    static FrameValue indirect(std::uint8_t size, int registerNumber, std::int32_t offset)
    {
        FrameValue result;
        result.m_union.indirect = {Tag::Indirect, size, registerNumber, offset};
        return result;
    }

    /// Read the values represented by the 'FrameValue' within 'frame'.
    /// 'U' is the interpretation of the value and must be a type greater or equal in size than the actual value.
    /// The resulting value is zero-extended and then bitcast to 'U'.
    template <class U>
    U read(const UnwindFrame& frame) const
    {
        static_assert(sizeof(U) <= sizeof(std::uint64_t), "cannot read values larger than 64 bit");
        static_assert(std::is_trivially_copyable_v<U>, "bitcast is only valid for trivially copyable types");

        std::uint64_t result{};
        switch (m_union.accessTag.tag)
        {
            case Tag::Constant: result = m_union.constant.constant; break;
            case Tag::Register: result = frame.getIntegerRegister(m_union.inRegister.registerNumber); break;
            case Tag::Direct:
                assert(sizeof(U) == sizeof(void*) && "type read must be equal to pointer size");
                result = frame.getIntegerRegister(m_union.direct.registerNumber) + m_union.direct.offset;
                break;
            case Tag::Indirect:
            {
                assert(sizeof(U) >= m_union.indirect.size && "type read must be large enough for the value");
                auto* ptr = reinterpret_cast<char*>(frame.getIntegerRegister(m_union.indirect.registerNumber)
                                                    + m_union.indirect.offset);
                std::memcpy(&result, ptr, m_union.indirect.size);
                break;
            }
            default: llvm_unreachable("invalid tag");
        }
        return llvm::bit_cast<U>(static_cast<NextSizedUInt<U>>(result));
    }

    /// Overload of 'read' for strongly typed 'FrameValue's, interpreting the value as 'T'.
    T read(const UnwindFrame& frame) const requires(!std::is_void_v<T>)
    {
        return read<T>(frame);
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
template <std::invocable<UnwindFrame&> F>
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

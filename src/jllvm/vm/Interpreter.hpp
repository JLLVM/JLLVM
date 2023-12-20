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

#include <jllvm/class/ByteCodeIterator.hpp>
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/support/BitArrayRef.hpp>
#include <jllvm/support/Bytes.hpp>

#include <cstdint>

namespace jllvm
{

class VirtualMachine;

/// Types that are class2 java types.
template <class T>
concept InterpreterClass2 = llvm::is_one_of<T, std::int64_t, std::uint64_t, double>::value;

/// Possible primitive types of values in the interpreter. Note that this includes both signed and unsigned variants
/// of integer types as the most suitable variant is dependent on the operation. E.g. if wraparound semantics are
/// desirable, doing calculations with unsigned type should be done to avoid undefined behaviour in C++.
/// Signed integer types can be assumed to be two's complement.
template <class T>
concept InterpreterPrimitive = llvm::is_one_of<T, std::int32_t, std::uint32_t, float>::value || InterpreterClass2<T>;

/// Possible types of values in the interpreter. These are all the primitive types with the addition of pointers to
/// Java objects (references).
template <class T>
concept InterpreterValue =
    InterpreterPrimitive<T> || (std::is_pointer_v<T> && std::derived_from<std::remove_pointer_t<T>, ObjectInterface>);

/// Context used in the execution of one Java frame. It incorporates and contains convenience methods for interacting
/// with local variables and the operand stack.
class InterpreterContext
{
    std::uint16_t& m_topOfStack;
    std::uint64_t* m_operandStack;
    std::uint64_t* m_operandGCMask;
    std::uint64_t* m_localVariables;
    std::uint64_t* m_localVariableGCMask;

    /// Returns true if 'T' is a reference to the Java heap.
    template <InterpreterValue T>
    constexpr static bool isReference()
    {
        return !InterpreterPrimitive<T>;
    }

    /// Sets the corresponding bit in 'mask' at index to 'value'.
    static void setMaskBit(std::uint64_t* mask, std::size_t index, bool value)
    {
        // While we cannot permanently keep around a 'MutableBitArrayRef' unless wastefully storing the size,
        // we can reuse its dereferencing implementation with a proper upper bound.
        MutableBitArrayRef(mask, index + 1)[index] = value;
    }

public:
    /// Creates a new 'InterpreterContext' from the given parameters. 'topOfStack' is a reference that is always kept
    /// up to date as the current top of stack. The two 'gc' mask parameters are used as bitsets and have their "i"th
    /// bit always set to true if the corresponding operand stack slot or local variable contains a Java reference.
    /// All the pointers passed here are not taken ownership of and must be allocated externally and valid while the
    /// 'InterpreterContext' is still in use.
    InterpreterContext(std::uint16_t& topOfStack, std::uint64_t* operandStack, std::uint64_t* operandGCMask,
                       std::uint64_t* localVariables, std::uint64_t* localVariableGCMask)
        : m_topOfStack(topOfStack),
          m_operandStack(operandStack),
          m_operandGCMask(operandGCMask),
          m_localVariables(localVariables),
          m_localVariableGCMask(localVariableGCMask)
    {
    }

    /// Pushes a value of type 'T' to the operand stack.
    template <InterpreterValue T>
    void push(T value)
    {
        pushRaw(llvm::bit_cast<NextSizedUInt<T>>(value), isReference<T>());
        if constexpr (InterpreterClass2<T>)
        {
            // "overwrite" the operand stack after as well.
            pushRaw(0, isReference<T>());
        }
    }

    /// Pushes the value into the top operand stack slot.
    /// This method operates on operand slots rather than 'InterpreterValue' as 'push' does.
    /// This notably has different behaviour for types such as 'long' or 'double'.
    void pushRaw(std::uint64_t value, bool isReference)
    {
        m_operandStack[m_topOfStack] = value;
        setMaskBit(m_operandGCMask, m_topOfStack, isReference);
        m_topOfStack++;
    }

    /// A raw value consisting of the value and a boolean denoting whether the type is a reference type.
    using RawValue = std::pair<std::uint64_t, bool>;

    void pushRaw(RawValue pair)
    {
        pushRaw(pair.first, pair.second);
    }

    /// Pops the top value of type 'T' from the operand stack.
    template <InterpreterValue T>
    T pop()
    {
        if constexpr (InterpreterClass2<T>)
        {
            popRaw();
        }
        return llvm::bit_cast<T>(static_cast<NextSizedUInt<T>>(popRaw().first));
    }

    /// Pops the top-most operand stack slot from the stack.
    RawValue popRaw()
    {
        assert(m_topOfStack != 0 && "bottom of stack already reached");
        bool isReference = BitArrayRef(m_operandGCMask, m_topOfStack)[--m_topOfStack];
        std::uint64_t copy = m_operandStack[m_topOfStack];
        return {copy, isReference};
    }

    /// Sets the local 'index' to the given 'value'.
    template <InterpreterValue T>
    void setLocal(std::uint16_t index, T value)
    {
        std::memcpy(m_localVariables + index, &value, sizeof(T));
        setMaskBit(m_localVariableGCMask, index, isReference<T>());
        if constexpr (InterpreterClass2<T>)
        {
            // overwrite local variable after.
            setMaskBit(m_localVariableGCMask, index + 1, isReference<T>());
        }
    }

    /// Gets the value of the local 'index' and interprets it as 'T'.
    template <InterpreterValue T>
    T getLocal(std::uint16_t index) const
    {
        T result;
        std::memcpy(&result, m_localVariables + index, sizeof(T));
        return result;
    }
};

/// Interpreter instance containing all global state of the interpreter.
class Interpreter
{
    VirtualMachine& m_virtualMachine;
    /// Enable OSR from the interpreter into the JIT if the method is hot enough.
    bool m_enableOSR;

    /// Returns the class object referred to by 'index' within 'classFile', loading it if necessary.
    ClassObject* getClassObject(const ClassFile& classFile, PoolIndex<ClassInfo> index);

    /// Returns the class object, field name and type referred to by 'index' within 'classFile'.
    std::tuple<ClassObject*, llvm::StringRef, FieldType> getFieldInfo(const ClassFile& classFile,
                                                                      PoolIndex<FieldRefInfo> index);

    /// Replaces the current interpreter frame with a compiled frame. This should only be called from within
    /// 'executeMethod' when called from the 'jllvm_interpreter' implementation in 'VirtualMachine'.
    [[noreturn]] void escapeToJIT();

public:
    explicit Interpreter(VirtualMachine& virtualMachine, bool enableOSR)
        : m_virtualMachine(virtualMachine), m_enableOSR(enableOSR)
    {
    }

    /// Method called to start executing 'method' at the given 'offset' with the given 'context'. Both the context and
    /// offset are kept up-to-date during execution with the current local variables, operand stack and offset being
    /// executed.
    /// Returns the result of the method bitcast to an uint64_t.
    std::uint64_t executeMethod(const Method& method, std::uint16_t& offset, InterpreterContext& context);
};
} // namespace jllvm

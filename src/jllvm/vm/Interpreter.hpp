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
#include <jllvm/materialization/JIT2InterpreterLayer.hpp>
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/support/BitArrayRef.hpp>
#include <jllvm/support/Bytes.hpp>

#include <cstdint>

#include "OSRState.hpp"
#include "Runtime.hpp"

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

    /// Pushes a value of the type give by 'descriptor' to the operand stack.
    void push(std::uint64_t value, FieldType descriptor)
    {
        pushRaw(value, descriptor.isReference());
        if (descriptor.isWide())
        {
            pushRaw(0, descriptor.isReference());
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

    /// Pushes a raw value pair of type 'T' to the operand stack.
    template <InterpreterValue T>
    void pushAsRaw(RawValue pair)
    {
        pushRaw(pair);
        if constexpr (InterpreterClass2<T>)
        {
            // "overwrite" the operand stack after as well.
            pushRaw(0, pair.second);
        }
    }

    void pushRaw(RawValue pair)
    {
        pushRaw(pair.first, pair.second);
    }

    /// Pops the top value of type 'T' from the operand stack.
    template <InterpreterValue T>
    T pop()
    {
        return llvm::bit_cast<T>(static_cast<NextSizedUInt<T>>(popAsRaw<T>().first));
    }

    /// Pops the top value of the type given by 'descriptor' from the operand stack.
    std::uint64_t pop(FieldType descriptor)
    {
        if (descriptor.isWide())
        {
            popRaw();
        }
        return popRaw().first;
    }

    /// Pops the top value of type 'T' as a raw value pair from the operand stack.
    template <InterpreterValue T>
    RawValue popAsRaw()
    {
        if constexpr (InterpreterClass2<T>)
        {
            popRaw();
        }
        return popRaw();
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

    /// Sets the local 'index' to the given raw value pair.
    template <InterpreterValue T>
    void setLocalAsRaw(std::uint16_t index, RawValue pair)
    {
        m_localVariables[index] = pair.first;
        setMaskBit(m_localVariableGCMask, index, pair.second);
        if constexpr (InterpreterClass2<T>)
        {
            // overwrite local variable after.
            setMaskBit(m_localVariableGCMask, index + 1, pair.second);
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

    /// Gets the raw value pair of the local 'index'.
    RawValue getLocalRaw(std::uint16_t index) const
    {
        bool isReference = BitArrayRef(m_localVariableGCMask, index + 1)[index];
        std::uint64_t copy = m_localVariables[index];
        return {copy, isReference};
    }

    /// Pops arguments from the stack matching a call to a possibly static method of type 'methodType'.
    /// Returns a view to the operands that were just popped where the last element in the view is the old top of the
    /// stack.
    ///
    /// Important note: The view is only valid until the next push to the operand stack. Furthermore, Garbage Collection
    /// will not find any references contained within the view. It is therefore illegal to access the view after garbage
    /// collection may occurred.
    llvm::ArrayRef<std::uint64_t> viewAndPopArguments(MethodType methodType, bool isStatic)
    {
        std::size_t size = isStatic ? 0 : 1;
        for (FieldType fieldType : methodType.parameters())
        {
            size++;
            if (fieldType.isWide())
            {
                size++;
            }
        }
        m_topOfStack -= size;
        return {m_operandStack + m_topOfStack, size};
    }
};

/// Interpreter instance containing all global state of the interpreter.
class Interpreter : public OSRTarget
{
    VirtualMachine& m_virtualMachine;
    /// Number of backedges before the Interpreter performs OSR into the JIT.
    std::uint64_t m_backEdgeThreshold;

    /// Single entry for use in 'm_interpreterCCSymbols' as an implementation for ALL methods.
    std::uint64_t (*m_interpreterEntry)(const Method*, const std::uint64_t*){};
    /// OSR Entry for frames with interpreter calling convention.
    void* m_interpreterInterpreterCCOSREntry;
    /// OSR Entry for frames with JIT calling convention returning a reference type.
    void* m_interpreterJITCCOSREntryReferenceReturn;
    /// OSR Entries for frames with JIT calling convention returning a base type.
    std::array<void*, (BaseType::MaxValue - BaseType::MinValue) + 1> m_interpreterJITCCOSREntries{};

    llvm::orc::JITDylib& m_jit2InterpreterSymbols;
    llvm::orc::JITDylib& m_interpreterCCSymbols;

    JIT2InterpreterLayer m_compiled2InterpreterLayer;

    /// Returns the class object referred to by 'index' within 'classFile', loading it if necessary.
    ClassObject* getClassObject(const ClassFile& classFile, PoolIndex<ClassInfo> index);

    ClassObject* getClassObject(const ClassFile& classFile, ClassInfo classInfo);

    /// Returns the class object, field name and type referred to by 'index' within 'classFile'.
    std::tuple<ClassObject*, llvm::StringRef, FieldType> getFieldInfo(const ClassFile& classFile,
                                                                      PoolIndex<FieldRefInfo> index);

    /// Replaces the current interpreter frame with a compiled frame. This should only be called from within
    /// 'executeMethod' when called from the 'jllvm_interpreter' implementation in 'VirtualMachine'.
    [[noreturn]] void escapeToJIT();

    static std::unique_ptr<std::uint64_t[]> createOSRBuffer(const Method& method, std::uint16_t byteCodeOffset,
                                                            llvm::ArrayRef<std::uint64_t> locals,
                                                            llvm::ArrayRef<std::uint64_t> operandStack,
                                                            BitArrayRef<> localsGCMask,
                                                            BitArrayRef<> operandStackGCMask);

    /// Method called to start executing 'method' at the given 'offset' with the given 'context'. Both the context and
    /// offset are kept up-to-date during execution with the current local variables, operand stack and offset being
    /// executed.
    /// Returns the result of the method bitcast to an uint64_t.
    std::uint64_t executeMethod(const Method& method, std::uint16_t& offset, InterpreterContext& context);

    /// Initializes 'm_interpreterEntry' by generating LLVM IR.
    void generateInterpreterEntry();

    /// Creates and returns an OSR Entry for the interpreter suitable for replacing a frame with the given calling
    /// convention and return type.
    void* generateOSREntry(FieldType returnType, CallingConvention callingConvention);

public:
    explicit Interpreter(VirtualMachine& virtualMachine, std::uint64_t backEdgeThreshold);

    void add(const Method& method) override;

    llvm::orc::JITDylib& getJITCCDylib() override
    {
        return m_jit2InterpreterSymbols;
    }

    llvm::orc::JITDylib& getInterpreterCCDylib() override
    {
        return m_interpreterCCSymbols;
    }

    bool canExecute(const Method& method) const override
    {
        return !(method.isNative() || method.isAbstract());
    }

    void* getOSREntry(const Method& method, std::uint16_t byteCodeOffset, CallingConvention callingConvention) override;

    OSRState createOSRStateFromInterpreterFrame(InterpreterFrame frame) override;

    OSRState createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                               Throwable* throwable) override;
};
} // namespace jllvm

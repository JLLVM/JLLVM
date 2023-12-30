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

#include "Interpreter.hpp"

#include <jllvm/class/ByteCodeIterator.hpp>

#include "VirtualMachine.hpp"

jllvm::Interpreter::Interpreter(VirtualMachine& virtualMachine, bool enableOSR)
    : m_virtualMachine(virtualMachine),
      m_enableOSR(enableOSR),
      m_jit2InterpreterSymbols(
          m_virtualMachine.getRuntime().getJITCCDylib().getExecutionSession().createBareJITDylib("<jit2interpreter>")),
      m_compiled2InterpreterLayer(virtualMachine.getRuntime().getInterner(),
                                  virtualMachine.getRuntime().getLLVMIRLayer(),
                                  virtualMachine.getRuntime().getDataLayout()),
      m_interpreterOSRLayer(m_compiled2InterpreterLayer.getInterner(), m_compiled2InterpreterLayer.getBaseLayer(),
                            m_compiled2InterpreterLayer.getDataLayout())
{
    m_jit2InterpreterSymbols.addToLinkOrder(virtualMachine.getRuntime().getClassAndMethodObjectsDylib());
    m_jit2InterpreterSymbols.addToLinkOrder(virtualMachine.getRuntime().getCLibDylib());

    m_virtualMachine.getRuntime().addImplementationSymbols(
        m_jit2InterpreterSymbols,
        std::pair{"jllvm_interpreter",
                  [&](const Method* method, std::uint16_t* byteCodeOffset, std::uint16_t* topOfStack,
                      std::uint64_t* operandStack, std::uint64_t* operandGCMask, std::uint64_t* localVariables,
                      std::uint64_t* localVariablesGCMask)
                  {
                      InterpreterContext context(*topOfStack, operandStack, operandGCMask, localVariables,
                                                 localVariablesGCMask);
                      return executeMethod(*method, *byteCodeOffset, context);
                  }},
        std::pair{"jllvm_osr_frame_delete", [](const std::uint64_t* osrFrame) { delete[] osrFrame; }});
}

namespace
{

void allowDuplicateDefinitions(llvm::Error&& error)
{
    llvm::handleAllErrors(std::move(error), [](const llvm::orc::DuplicateDefinition&) {});
}

/// Tag returned when interpreting an instruction to jump to a new bytecode offset.
struct SetPC
{
    std::uint16_t newPC;
};

/// Tag returned when interpreting an instruction to continue to the next instruction in the bytecode.
struct NextPC
{
};

/// Tag returned when interpreting an instruction to stop interpretation and return a result.
struct ReturnValue
{
    std::uint64_t value;

    template <jllvm::InterpreterValue T>
    ReturnValue(T value) : value(llvm::bit_cast<jllvm::NextSizedUInt<T>>(value))
    {
    }
};

using InstructionResult = swl::variant<SetPC, NextPC, ReturnValue>;

} // namespace

jllvm::ClassObject* jllvm::Interpreter::getClassObject(const ClassFile& classFile, PoolIndex<ClassInfo> index)
{
    return getClassObject(classFile, *index.resolve(classFile));
}

jllvm::ClassObject* jllvm::Interpreter::getClassObject(const ClassFile& classFile, ClassInfo classInfo)
{
    llvm::StringRef className = classInfo.nameIndex.resolve(classFile)->text;
    return &m_virtualMachine.getClassLoader().forName(FieldType::fromMangled(className));
}

std::tuple<jllvm::ClassObject*, llvm::StringRef, jllvm::FieldType>
    jllvm::Interpreter::getFieldInfo(const ClassFile& classFile, PoolIndex<FieldRefInfo> index)
{
    const FieldRefInfo* refInfo = index.resolve(classFile);
    const NameAndTypeInfo* nameAndTypeInfo = refInfo->nameAndTypeIndex.resolve(classFile);
    llvm::StringRef fieldName = nameAndTypeInfo->nameIndex.resolve(classFile)->text;
    FieldType descriptor(nameAndTypeInfo->descriptorIndex.resolve(classFile)->text);

    ClassObject* classObject = getClassObject(classFile, refInfo->classIndex);
    return {classObject, fieldName, descriptor};
}

void jllvm::Interpreter::escapeToJIT()
{
    m_virtualMachine.unwindJavaStack(
        [&](JavaFrame frame)
        {
            m_virtualMachine.getRuntime().doOnStackReplacement(
                frame,
                m_virtualMachine.getJIT().createOSRStateFromInterpreterFrame(llvm::cast<InterpreterFrame>(frame)));
        });
    llvm_unreachable("not possible");
}

namespace
{

using namespace jllvm;

/// Mapping of instruction 'T' to the type it operates on.
template <class T>
struct InstructionElementType;

template <OperatesOnReferences T>
struct InstructionElementType<T>
{
    using type = ObjectInterface*;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = type;
};

template <OperatesOnIntegers T>
struct InstructionElementType<T>
{
    using type = std::uint32_t;
    using unsigned_type = type;
    using signed_type = std::int32_t;
    using field_type = signed_type;
};

template <OperatesOnFloat T>
struct InstructionElementType<T>
{
    using type = float;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = signed_type;
};

template <OperatesOnLong T>
struct InstructionElementType<T>
{
    using type = std::uint64_t;
    using unsigned_type = type;
    using signed_type = std::int64_t;
    using field_type = signed_type;
};

template <OperatesOnDouble T>
struct InstructionElementType<T>
{
    using type = double;
    using unsigned_type = type;
    using signed_type = type;
    using field_type = signed_type;
};

template <OperatesOnByte T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::uint8_t;
};

template <OperatesOnShort T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::int16_t;
};

template <OperatesOnChar T>
struct InstructionElementType<T>
{
    using type = std::int32_t;
    using field_type = std::uint16_t;
};

/// Function object implementing the comparison operator performed by the instruction 'T'.
template <class T>
struct ComparisonOperator;

template <DoesEqual T>
struct ComparisonOperator<T> : std::equal_to<>
{
};

template <DoesNotEqual T>
struct ComparisonOperator<T> : std::not_equal_to<>
{
};

template <DoesLessThan T>
struct ComparisonOperator<T> : std::less<>
{
};

template <DoesGreaterEqual T>
struct ComparisonOperator<T> : std::greater_equal<>
{
};

template <DoesGreaterThan T>
struct ComparisonOperator<T> : std::greater<>
{
};

template <DoesLessEqual T>
struct ComparisonOperator<T> : std::less_equal<>
{
};

/// Maps the conversion instruction 'T' to the source, target and target type on the stack that the instruction
/// converts between.
template <DoesConversion T>
struct ConversionOperator;

template <ConvertsToFloat T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = float;
    using stack_type = target_type;
};

template <ConvertsToDouble T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = double;
    using stack_type = target_type;
};

template <ConvertsToInt T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = std::int32_t;
    using stack_type = target_type;
};

template <ConvertsToLong T>
struct ConversionOperator<T>
{
    using source_type = typename InstructionElementType<T>::signed_type;
    using target_type = std::int64_t;
    using stack_type = target_type;
};

template <>
struct ConversionOperator<I2B>
{
    using source_type = std::int32_t;
    using target_type = std::int8_t;
    using stack_type = source_type;
};

template <>
struct ConversionOperator<I2C>
{
    using source_type = std::int32_t;
    using target_type = std::uint16_t;
    using stack_type = source_type;
};

template <>
struct ConversionOperator<I2S>
{
    using source_type = std::int32_t;
    using target_type = std::int16_t;
    using stack_type = source_type;
};

/// Struct used to implement instructions with generic implementations parameterized on their operand types.
struct MultiTypeImpls
{
    VirtualMachine& virtualMachine;
    InterpreterContext& context;

    template <IsAdd T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs + rhs);
        return {};
    }

    template <IsSub T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs - rhs);
        return {};
    }

    template <IsNeg T>
    NextPC operator()(T) const
    {
        context.push(-context.pop<typename InstructionElementType<T>::unsigned_type>());
        return {};
    }

    template <IsMul T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs * rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, FDiv, DDiv>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::type>();
        auto lhs = context.pop<typename InstructionElementType<T>::type>();
        context.push(lhs / rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, IDiv, LDiv>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::signed_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::signed_type>();
        if (rhs == 0)
        {
            // TODO: Throw ArithmeticException.
            llvm::report_fatal_error("Throwing ArithmeticException is not yet implemented");
        }
        context.push(lhs / rhs);
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, FRem, DRem>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::type>();
        auto lhs = context.pop<typename InstructionElementType<T>::type>();
        context.push(std::fmod(lhs, rhs));
        return {};
    }

    template <class T>
    NextPC operator()(T) const requires llvm::is_one_of<T, IRem, LRem>::value
    {
        auto rhs = context.pop<typename InstructionElementType<T>::signed_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::signed_type>();
        if (rhs == 0)
        {
            // TODO: Throw ArithmeticException.
            llvm::report_fatal_error("Throwing ArithmeticException is not yet implemented");
        }
        context.push(lhs % rhs);
        return {};
    }

    template <IsOr T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs | rhs);
        return {};
    }

    template <IsAnd T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs & rhs);
        return {};
    }

    template <IsXor T>
    NextPC operator()(T) const
    {
        auto rhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        auto lhs = context.pop<typename InstructionElementType<T>::unsigned_type>();
        context.push(lhs ^ rhs);
        return {};
    }

    template <IsShl T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::unsigned_type;
        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<type>::digits - 1;
        context.push(lhs << (rhs & mask));
        return {};
    }

    template <IsShr T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::signed_type;

        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<typename InstructionElementType<T>::unsigned_type>::digits - 1;
        context.push(lhs >> (rhs & mask));
        return {};
    }

    template <IsUShr T>
    NextPC operator()(T) const
    {
        using type = typename InstructionElementType<T>::unsigned_type;

        auto rhs = context.pop<std::uint32_t>();
        auto lhs = context.pop<type>();
        constexpr auto mask = std::numeric_limits<type>::digits - 1;
        context.push(lhs >> (rhs & mask));
        return {};
    }

    template <IsIfCmp T>
    InstructionResult operator()(T instruction) const
    {
        auto value2 = context.pop<typename InstructionElementType<T>::signed_type>();
        auto value1 = context.pop<typename InstructionElementType<T>::signed_type>();
        if (ComparisonOperator<T>{}(value1, value2))
        {
            return SetPC{static_cast<std::uint16_t>(instruction.offset + instruction.target)};
        }
        return NextPC{};
    }

    template <IsIf T>
    InstructionResult operator()(T instruction) const
    {
        auto value = context.pop<typename InstructionElementType<T>::signed_type>();
        // NOLINTNEXTLINE(*-use-nullptr): clang-tidy warns the use of '0' rather than 'nullptr' despite being templated.
        if (ComparisonOperator<T>{}(value, static_cast<decltype(value)>(0)))
        {
            return SetPC{static_cast<std::uint16_t>(instruction.offset + instruction.target)};
        }
        return NextPC{};
    }

    template <DoesConversion T>
    NextPC operator()(T) const
    {
        using source_type = typename ConversionOperator<T>::source_type;
        using target_type = typename ConversionOperator<T>::target_type;
        using stack_type = typename ConversionOperator<T>::stack_type;

        auto value = context.pop<source_type>();
        if constexpr (!std::is_floating_point_v<source_type> || !std::is_integral_v<target_type>)
        {
            // C++s builtin conversions implement the semantics required if not converting from a float to an integer.
            context.push<stack_type>(static_cast<target_type>(value));
        }
        else
        {
            auto trunc = std::trunc(value);
            if (std::isnan(trunc))
            {
                // NaNs convert to 0.
                context.push<stack_type>(0);
            }
            else if (trunc >= static_cast<source_type>(std::numeric_limits<target_type>::min())
                     && trunc <= static_cast<source_type>(std::numeric_limits<target_type>::max()))
            {
                // If after rounding the value fits within the target type, use it as is.
                context.push<stack_type>(static_cast<target_type>(trunc));
            }
            else if (trunc < 0)
            {
                // Otherwise, the float maps to either the largest or smallest integer value.
                context.push<stack_type>(std::numeric_limits<target_type>::min());
            }
            else
            {
                context.push<stack_type>(std::numeric_limits<target_type>::max());
            }
        }

        return NextPC{};
    }

    template <IsFPCmp T>
    NextPC operator()(T) const
    {
        auto value2 = context.pop<typename InstructionElementType<T>::type>();
        auto value1 = context.pop<typename InstructionElementType<T>::type>();
        if (value1 > value2)
        {
            context.push<std::int32_t>(1);
        }
        else if (value1 == value2)
        {
            context.push<std::int32_t>(0);
        }
        else if (value1 < value2)
        {
            context.push<std::int32_t>(-1);
        }
        else
        {
            // At least one of the operands is a NaN leading to all comparisons to yield false. Depending on the
            // instruction, either 1 or -1 is pushed.
            if constexpr (llvm::is_one_of<T, FCmpG, DCmpG>{})
            {
                context.push<std::int32_t>(1);
            }
            else
            {
                context.push<std::int32_t>(-1);
            }
        }
        return {};
    }

    template <IsLoad T>
    NextPC operator()(T load) const
    {
        context.push(context.getLocal<typename InstructionElementType<T>::type>(load.index));
        return {};
    }

    template <IsLoad0 T>
    NextPC operator()(T) const
    {
        context.push(context.getLocal<typename InstructionElementType<T>::type>(0));
        return {};
    }

    template <IsLoad1 T>
    NextPC operator()(T) const
    {
        context.push(context.getLocal<typename InstructionElementType<T>::type>(1));
        return {};
    }

    template <IsLoad2 T>
    NextPC operator()(T) const
    {
        context.push(context.getLocal<typename InstructionElementType<T>::type>(2));
        return {};
    }

    template <IsLoad3 T>
    NextPC operator()(T) const
    {
        context.push(context.getLocal<typename InstructionElementType<T>::type>(3));
        return {};
    }

    template <IsALoad T>
    InstructionResult operator()(T) const
    {
        auto index = context.pop<std::int32_t>();
        auto* array = context.pop<Array<typename InstructionElementType<T>::field_type>*>();
        if (!array)
        {
            virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
        }
        if (index < 0 || index >= array->size())
        {
            virtualMachine.throwArrayIndexOutOfBoundsException(index, array->size());
        }
        context.push<typename InstructionElementType<T>::type>((*array)[index]);
        return NextPC{};
    }

    template <IsStore T>
    NextPC operator()(T store) const
    {
        context.setLocal(store.index, context.pop<typename InstructionElementType<T>::type>());
        return {};
    }

    template <IsStore0 T>
    NextPC operator()(T) const
    {
        context.setLocal(0, context.pop<typename InstructionElementType<T>::type>());
        return {};
    }

    template <IsStore1 T>
    NextPC operator()(T) const
    {
        context.setLocal(1, context.pop<typename InstructionElementType<T>::type>());
        return {};
    }

    template <IsStore2 T>
    NextPC operator()(T) const
    {
        context.setLocal(2, context.pop<typename InstructionElementType<T>::type>());
        return {};
    }

    template <IsStore3 T>
    NextPC operator()(T) const
    {
        context.setLocal(3, context.pop<typename InstructionElementType<T>::type>());
        return {};
    }

    template <IsConst0 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(0);
        return {};
    }

    template <IsConst1 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(1);
        return {};
    }

    template <IsConst2 T>
    NextPC operator()(T) const
    {
        context.push<typename InstructionElementType<T>::type>(2);
        return {};
    }

    /// Implementation for all return instructions that return a value with the exception of 'ireturn'.
    /// 'ireturn' has special semantics if the return type of the method is an integer type other than 'int'.
    template <IsReturnValue T>
    ReturnValue operator()(T) const requires(!std::same_as<T, IReturn>)
    {
        return ReturnValue(context.pop<typename InstructionElementType<T>::type>());
    }

    template <IsAStore T>
    NextPC operator()(T) const
    {
        auto value = context.pop<typename InstructionElementType<T>::type>();
        auto index = context.pop<std::int32_t>();
        auto* array = context.pop<Array<typename InstructionElementType<T>::field_type>*>();
        if (!array)
        {
            virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
        }
        if (index < 0 || index >= array->size())
        {
            virtualMachine.throwArrayIndexOutOfBoundsException(index, array->size());
        }
        (*array)[index] = value;
        return {};
    }
};

} // namespace

std::uint64_t jllvm::Interpreter::executeMethod(const Method& method, std::uint16_t& offset,
                                                InterpreterContext& context)
{
    const ClassFile& classFile = *method.getClassObject()->getClassFile();
    Code* code = method.getMethodInfo().getAttributes().find<Code>();
    assert(code && "method being interpreted must have code");
    llvm::ArrayRef<char> codeArray = code->getCode();
    auto curr = ByteCodeIterator(codeArray.data(), offset);
    MethodType methodType = method.getType();

    while (true)
    {
        // Update the current offset to the new instruction.
        offset = curr.getOffset();
        InstructionResult result = match(
            *curr, MultiTypeImpls{m_virtualMachine, context},
            [&](ANewArray aNewArray)
            {
                auto count = context.pop<std::int32_t>();
                if (count < 0)
                {
                    m_virtualMachine.throwNegativeArraySizeException(count);
                }
                ClassObject* componentType = getClassObject(classFile, aNewArray.index);
                ClassObject& arrayType =
                    m_virtualMachine.getClassLoader().forName(ArrayType(componentType->getDescriptor()));
                auto* array = m_virtualMachine.getGC().allocate<AbstractArray>(&arrayType, count);
                context.push(array);
                return NextPC{};
            },
            [&](AThrow) -> InstructionResult
            {
                auto* exception = context.pop<ObjectInterface*>();
                if (!exception)
                {
                    m_virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
                }
                // Verifier checks that the exception is an instance of 'Throwable' rather than performing it at
                // runtime.
                m_virtualMachine.throwJavaException(static_cast<Throwable*>(exception));
            },
            [&](ArrayLength)
            {
                auto* array = context.pop<AbstractArray*>();
                if (!array)
                {
                    m_virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
                }
                context.push<std::uint32_t>(array->size());
                return NextPC{};
            },
            [&](AConstNull)
            {
                context.push<ObjectInterface*>(nullptr);
                return NextPC{};
            },
            [&](BIPush biPush)
            {
                context.push<std::int32_t>(biPush.value);
                return NextPC{};
            },
            [&](CheckCast checkCast)
            {
                auto* object = context.pop<ObjectInterface*>();
                context.push(object);
                if (!object)
                {
                    return NextPC{};
                }

                ClassObject* classObject = getClassObject(classFile, checkCast.index);
                if (object->instanceOf(classObject))
                {
                    return NextPC{};
                }

                m_virtualMachine.throwClassCastException(object, classObject);
            },
            [&](Dup)
            {
                InterpreterContext::RawValue value = context.popRaw();
                context.pushRaw(value);
                context.pushRaw(value);
                return NextPC{};
            },
            [&](DupX1)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](DupX2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2X1)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](Dup2X2)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                InterpreterContext::RawValue value3 = context.popRaw();
                InterpreterContext::RawValue value4 = context.popRaw();
                context.pushRaw(value2);
                context.pushRaw(value1);
                context.pushRaw(value4);
                context.pushRaw(value3);
                context.pushRaw(value2);
                context.pushRaw(value1);
                return NextPC{};
            },
            [&](GetField getField)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getField.index);

                const Field* field = classObject->getInstanceField(fieldName, descriptor);
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    m_virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
                }

                std::uint64_t value{};
                std::memcpy(&value, reinterpret_cast<char*>(object) + field->getOffset(), descriptor.sizeOf());
                context.push(value, descriptor);
                return NextPC{};
            },
            [&](GetStatic getStatic)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getStatic.index);

                m_virtualMachine.initialize(*classObject);
                const Field* field = classObject->getStaticField(fieldName, descriptor);

                std::uint64_t value{};
                std::memcpy(&value, field->getAddressOfStatic(), descriptor.sizeOf());
                context.push(value, descriptor);
                return NextPC{};
            },
            [&](OneOf<Goto, GotoW> gotoInst)
            { return SetPC{static_cast<std::uint16_t>(gotoInst.offset + gotoInst.target)}; },
            [&](IConst3)
            {
                context.push<std::int32_t>(3);
                return NextPC{};
            },
            [&](IConst4)
            {
                context.push<std::int32_t>(4);
                return NextPC{};
            },
            [&](IConst5)
            {
                context.push<std::int32_t>(5);
                return NextPC{};
            },
            [&](IConstM1)
            {
                context.push<std::int32_t>(-1);
                return NextPC{};
            },
            [&](IInc iInc)
            {
                context.setLocal(iInc.index,
                                 static_cast<std::int32_t>(iInc.byte) + context.getLocal<std::uint32_t>(iInc.index));
                return NextPC{};
            },
            [&](IReturn)
            {
                auto value = context.pop<std::uint32_t>();
                switch (get<BaseType>(methodType.returnType()).getValue())
                {
                    case BaseType::Boolean: value &= 0b1; break;
                    case BaseType::Char: value = static_cast<std::int32_t>(static_cast<std::uint16_t>(value)); break;
                    case BaseType::Byte: value = static_cast<std::int32_t>(static_cast<std::int8_t>(value)); break;
                    case BaseType::Short: value = static_cast<std::int32_t>(static_cast<std::int16_t>(value)); break;
                    case BaseType::Int: break;
                    case BaseType::Long:
                    case BaseType::Void:
                    case BaseType::Float:
                    case BaseType::Double:
                    default: llvm_unreachable("not possible");
                }
                return ReturnValue(value);
            },
            [&](InstanceOf instanceOf)
            {
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    context.push<std::int32_t>(0);
                    return NextPC{};
                }

                ClassObject* classObject = getClassObject(classFile, instanceOf.index);
                context.push<std::int32_t>(object->instanceOf(classObject));
                return NextPC{};
            },
            [&](LCmp)
            {
                auto value2 = context.pop<std::int64_t>();
                auto value1 = context.pop<std::int64_t>();
                if (value1 > value2)
                {
                    context.push<std::int32_t>(1);
                }
                else if (value1 == value2)
                {
                    context.push<std::int32_t>(0);
                }
                else
                {
                    context.push<std::int32_t>(-1);
                }
                return NextPC{};
            },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(classFile), [&](const IntegerInfo* integerInfo) { context.push(integerInfo->value); },
                    [&](const FloatInfo* floatInfo) { context.push(floatInfo->value); },
                    [&](const LongInfo* longInfo) { context.push(longInfo->value); },
                    [&](const DoubleInfo* doubleInfo) { context.push(doubleInfo->value); },
                    [&](const ClassInfo* classInfo) { context.push(getClassObject(classFile, *classInfo)); },
                    [&](const StringInfo* stringInfo)
                    {
                        llvm::StringRef utf8String = stringInfo->stringValue.resolve(classFile)->text;
                        context.push(m_virtualMachine.getStringInterner().intern(utf8String));
                    },
                    [&](const auto*) { escapeToJIT(); });
                return NextPC{};
            },
            [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
            {
                auto index = context.pop<std::int32_t>();
                auto result =
                    llvm::lower_bound(switchOp.matchOffsetsPairs, index,
                                      [](const auto& pair, std::int32_t value) { return pair.first < value; });
                if (result == switchOp.matchOffsetsPairs.end() || result->first != index)
                {
                    return SetPC{static_cast<std::uint16_t>(switchOp.offset + switchOp.defaultOffset)};
                }
                return SetPC{static_cast<std::uint16_t>(switchOp.offset + result->second)};
            },
            [&](OneOf<MonitorEnter, MonitorExit>)
            {
                // Pop object as is required by the instruction.
                // TODO: If we ever care about multi threading, this would require lazily creating a mutex and
                //  (un)locking it.
                if (!context.pop<ObjectInterface*>())
                {
                    m_virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
                }
                return NextPC{};
            },
            [&](MultiANewArray multiANewArray)
            {
                GarbageCollector& gc = m_virtualMachine.getGC();
                ClassObject* classObject = getClassObject(classFile, multiANewArray.index);
                std::vector<std::int32_t> counts(multiANewArray.dimensions);

                std::generate(counts.rbegin(), counts.rend(), [&] { return context.pop<std::int32_t>(); });

                for (std::int32_t count : counts)
                {
                    if (count < 0)
                    {
                        m_virtualMachine.throwNegativeArraySizeException(count);
                    }
                };

                auto generateArray = [&](llvm::ArrayRef<std::int32_t> counts, ArrayType currentType,
                                         const auto generator) -> ObjectInterface*
                {
                    std::int32_t count = counts.front();
                    counts = counts.drop_front();
                    ClassObject& arrayType = m_virtualMachine.getClassLoader().forName(currentType);
                    GCUniqueRoot array = gc.root(gc.allocate<AbstractArray>(&arrayType, count));
                    if (!counts.empty())
                    {
                        auto outerArray = static_cast<GCRootRef<Array<>>>(array);
                        auto componentType = get<ArrayType>(currentType.getComponentType());
                        // necessary, because iterator for Arrays is not gc safe
                        for (std::uint32_t i : llvm::seq(0u, outerArray->size()))
                        {
                            // allocation must happen before indexing
                            ObjectInterface* innerArray = generator(counts, componentType, generator);
                            (*outerArray)[i] = innerArray;
                        }
                    }
                    return array;
                };

                context.push(generateArray(counts, get<ArrayType>(classObject->getDescriptor()), generateArray));

                return NextPC{};
            },
            [&](New newInst)
            {
                ClassObject* classObject = getClassObject(classFile, newInst.index);
                m_virtualMachine.initialize(*classObject);
                context.push(m_virtualMachine.getGC().allocate(classObject));
                return NextPC{};
            },
            [&](NewArray newArray)
            {
                auto count = context.pop<std::int32_t>();
                if (count < 0)
                {
                    m_virtualMachine.throwNegativeArraySizeException(count);
                }

                ClassObject& arrayType =
                    m_virtualMachine.getClassLoader().forName(ArrayType{BaseType{newArray.componentType}});
                auto* array = m_virtualMachine.getGC().allocate<AbstractArray>(&arrayType, count);
                context.push(array);
                return NextPC{};
            },
            [&](Nop) { return NextPC{}; },
            [&](Pop)
            {
                context.popRaw();
                return NextPC{};
            },
            [&](Pop2)
            {
                context.popRaw();
                context.popRaw();
                return NextPC{};
            },
            [&](PutField putField)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, putField.index);
                const Field* field = classObject->getInstanceField(fieldName, descriptor);

                std::uint64_t value = context.pop(descriptor);
                auto* object = context.pop<ObjectInterface*>();
                if (!object)
                {
                    m_virtualMachine.throwException("Ljava/lang/NullPointerException;", "()V");
                }

                std::memcpy(reinterpret_cast<char*>(object) + field->getOffset(), &value, descriptor.sizeOf());
                return NextPC{};
            },
            [&](PutStatic getStatic)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getStatic.index);

                m_virtualMachine.initialize(*classObject);
                Field* field = classObject->getStaticField(fieldName, descriptor);

                std::uint64_t value = context.pop(descriptor);
                std::memcpy(field->getAddressOfStatic(), &value, descriptor.sizeOf());
                return NextPC{};
            },
            [&](Return)
            {
                // "Noop" return value for void methods.
                return ReturnValue{0};
            },
            [&](SIPush siPush)
            {
                context.push<std::int32_t>(siPush.value);
                return NextPC{};
            },
            [&](Swap)
            {
                InterpreterContext::RawValue value1 = context.popRaw();
                InterpreterContext::RawValue value2 = context.popRaw();
                context.pushRaw(value1);
                context.pushRaw(value2);
                return NextPC{};
            },
            [&](Wide wide)
            {
#define WIDE_LOAD_CASE(op)                                                            \
    case OpCodes::op:                                                                 \
    {                                                                                 \
        context.push(context.getLocal<InstructionElementType<op>::type>(wide.index)); \
        break;                                                                        \
    }

#define WIDE_STORE_CASE(op)                                                            \
    case OpCodes::op:                                                                  \
    {                                                                                  \
        context.setLocal(wide.index, context.pop<InstructionElementType<op>::type>()); \
        break;                                                                         \
    }
                switch (wide.opCode)
                {
                    WIDE_LOAD_CASE(ALoad)
                    WIDE_LOAD_CASE(DLoad)
                    WIDE_LOAD_CASE(FLoad)
                    WIDE_LOAD_CASE(ILoad)
                    WIDE_LOAD_CASE(LLoad)
                    WIDE_STORE_CASE(AStore)
                    WIDE_STORE_CASE(DStore)
                    WIDE_STORE_CASE(FStore)
                    WIDE_STORE_CASE(IStore)
                    WIDE_STORE_CASE(LStore)
                    case OpCodes::Ret:
                    {
                        // TODO: implement later
                        escapeToJIT();
                    }
                    case OpCodes::IInc:
                    {
                        context.setLocal(wide.index, static_cast<std::int32_t>(*wide.value)
                                                         + context.getLocal<std::uint32_t>(wide.index));
                        break;
                    }
                    default: llvm_unreachable("Invalid wide operation");
                }
#undef WIDE_LOAD_CASE
#undef WIDE_STORE_CASE

                return NextPC{};
            },
            [&](...) -> InstructionResult
            {
                // While the interpreter is not fully implemented, we escaped to JIT code that implements the
                // given bytecode instruction.
                // TODO: Remove this once interpreter implements all bytecodes.
                escapeToJIT();
            });

        if (auto* returnValue = get_if<ReturnValue>(&result))
        {
            return returnValue->value;
        }

        match(
            result, [](ReturnValue) {}, [&](NextPC) { ++curr; },
            [&](SetPC setPc) { curr = ByteCodeIterator(codeArray.data(), setPc.newPC); });
    }
}

void* Interpreter::getOSREntry(const Method& method, std::uint16_t byteCodeOffset)
{
    llvm::orc::SymbolStringPtr mangledName =
        m_interpreterOSRLayer.getInterner()(mangleOSRMethod(&method, byteCodeOffset));
    allowDuplicateDefinitions(m_interpreterOSRLayer.add(m_jit2InterpreterSymbols, &method, byteCodeOffset));

    llvm::JITEvaluatedSymbol osrMethod =
        llvm::cantFail(m_virtualMachine.getRuntime().getSession().lookup({&m_jit2InterpreterSymbols}, mangledName));
    return reinterpret_cast<void*>(osrMethod.getAddress());
}

OSRState Interpreter::createOSRStateFromInterpreterFrame(InterpreterFrame frame)
{
    return OSRState(*this, *frame.getByteCodeOffset(),
                    createOSRBuffer(*frame.getByteCodeOffset(), frame.readLocals(), frame.getOperandStack(),
                                    frame.getLocalsGCMask(), frame.getOperandStackGCMask()));
}

OSRState Interpreter::createOSRStateForExceptionHandler(JavaFrame frame, std::uint16_t handlerOffset,
                                                        Throwable* throwable)
{
    llvm::SmallVector<std::uint64_t> localsGcMask = frame.readLocalsGCMask();
    auto operandStackGCMask = std::initializer_list<std::uint64_t>{0b1};
    return OSRState(
        *this, handlerOffset,
        createOSRBuffer(
            handlerOffset, frame.readLocals(),
            /*operandStack=*/std::initializer_list<std::uint64_t>{reinterpret_cast<std::uint64_t>(throwable)},
            BitArrayRef(localsGcMask.data(), localsGcMask.size() * 64),
            BitArrayRef(data(operandStackGCMask), operandStackGCMask.size() * 64)));
}

std::unique_ptr<std::uint64_t[]> Interpreter::createOSRBuffer(std::uint16_t byteCodeOffset,
                                                              llvm::ArrayRef<std::uint64_t> locals,
                                                              llvm::ArrayRef<std::uint64_t> operandStack,
                                                              BitArrayRef<> localsGCMask,
                                                              BitArrayRef<> operandStackGCMask)
{
    std::size_t numLocals = llvm::size(locals);
    std::size_t numOperandStack = llvm::size(operandStack);

    auto buffer = std::make_unique<std::uint64_t[]>(1 + numLocals + numOperandStack + localsGCMask.numWords()
                                                    + operandStackGCMask.numWords());
    buffer[0] = byteCodeOffset | numOperandStack << 16;

    auto* outIter = llvm::copy(locals, std::next(buffer.get()));
    outIter = llvm::copy(operandStack, outIter);
    outIter = std::copy_n(localsGCMask.words_begin(), localsGCMask.numWords(), outIter);
    std::copy_n(operandStackGCMask.words_begin(), operandStackGCMask.numWords(), outIter);
    return buffer;
}

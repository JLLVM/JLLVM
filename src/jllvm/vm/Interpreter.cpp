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

namespace
{
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
} // namespace

jllvm::ClassObject* jllvm::Interpreter::getClassObject(const ClassFile& classFile, PoolIndex<ClassInfo> index)
{
    llvm::StringRef className = index.resolve(classFile)->nameIndex.resolve(classFile)->text;
    if (className.front() == '[')
    {
        return &m_virtualMachine.getClassLoader().forName(FieldType(className));
    }
    return &m_virtualMachine.getClassLoader().forName(ObjectType(className));
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
        { m_virtualMachine.getJIT().doI2JOnStackReplacement(llvm::cast<InterpreterFrame>(frame)); });
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
};

template <OperatesOnIntegers T>
struct InstructionElementType<T>
{
    using type = std::uint32_t;
    using unsigned_type = type;
    using signed_type = std::int32_t;
};

template <OperatesOnFloat T>
struct InstructionElementType<T>
{
    using type = float;
    using unsigned_type = type;
    using signed_type = type;
};

template <OperatesOnLong T>
struct InstructionElementType<T>
{
    using type = std::uint64_t;
    using unsigned_type = type;
    using signed_type = std::int64_t;
};

template <OperatesOnDouble T>
struct InstructionElementType<T>
{
    using type = double;
    using unsigned_type = type;
    using signed_type = type;
};

/// Struct used to implement instructions with generic implementations parameterized on their operand types.
struct MultiTypeImpls
{
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
        context.push(-context.pop<typename InstructionElementType<T>::signed_type>());
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

    using InstructionResult = swl::variant<SetPC, NextPC, ReturnValue>;

    while (true)
    {
        // Update the current offset to the new instruction.
        offset = curr.getOffset();
        InstructionResult result = match(
            *curr, MultiTypeImpls{context},
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
                context.pushRaw(value, descriptor.isReference());
                if (descriptor.isWide())
                {
                    context.pushRaw(0, descriptor.isReference());
                }
                return NextPC{};
            },
            [&](GetStatic getStatic)
            {
                auto [classObject, fieldName, descriptor] = getFieldInfo(classFile, getStatic.index);

                m_virtualMachine.initialize(*classObject);
                const Field* field = classObject->getStaticField(fieldName, descriptor);

                std::uint64_t value{};
                std::memcpy(&value, field->getAddressOfStatic(), descriptor.sizeOf());
                context.pushRaw(value, descriptor.isReference());
                if (descriptor.isWide())
                {
                    context.pushRaw(0, descriptor.isReference());
                }
                return NextPC{};
            },
            [&](IConst0)
            {
                context.push<std::int32_t>(0);
                return NextPC{};
            },
            [&](IConst1)
            {
                context.push<std::int32_t>(1);
                return NextPC{};
            },
            [&](IConst2)
            {
                context.push<std::int32_t>(2);
                return NextPC{};
            },
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
            [&](IReturn) { return ReturnValue(context.pop<std::uint32_t>()); },
            [&](LDC ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(classFile), [&](const IntegerInfo* integerInfo) { context.push(integerInfo->value); },
                    [&](const auto*) { escapeToJIT(); });
                return NextPC{};
            },
            [&](New newInst)
            {
                ClassObject* classObject = getClassObject(classFile, newInst.index);
                m_virtualMachine.initialize(*classObject);
                context.push(m_virtualMachine.getGC().allocate(classObject));
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

                std::uint64_t value = context.popRaw().first;
                if (descriptor.isWide())
                {
                    context.popRaw();
                }
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

                std::uint64_t value = context.popRaw().first;
                if (descriptor.isWide())
                {
                    context.popRaw();
                }

                std::memcpy(field->getAddressOfStatic(), &value, descriptor.sizeOf());
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
            [&](ByteCodeBase) -> InstructionResult
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

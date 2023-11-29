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

void jllvm::Interpreter::escapeToJIT()
{
    m_virtualMachine.unwindJavaStack(
        [&](JavaFrame frame) { m_virtualMachine.getJIT().doI2JOnStackReplacement(frame, *frame.getByteCodeOffset()); });
    llvm_unreachable("not possible");
}

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
            *curr,
            [&](IAdd)
            {
                auto lhs = context.pop<std::uint32_t>();
                auto rhs = context.pop<std::uint32_t>();
                context.push(lhs + rhs);
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
            [&](ILoad0)
            {
                context.push(context.getLocal<std::int32_t>(0));
                return NextPC{};
            },
            [&](ILoad1)
            {
                context.push(context.getLocal<std::int32_t>(1));
                return NextPC{};
            },
            [&](ILoad2)
            {
                context.push(context.getLocal<std::int32_t>(2));
                return NextPC{};
            },
            [&](ILoad3)
            {
                context.push(context.getLocal<std::int32_t>(3));
                return NextPC{};
            },
            [&](ILoad iLoad)
            {
                context.push(context.getLocal<std::int32_t>(iLoad.index));
                return NextPC{};
            },
            [&](IReturn) { return ReturnValue(context.pop<std::uint32_t>()); },
            [&](IStore0)
            {
                context.setLocal(0, context.pop<std::int32_t>());
                return NextPC{};
            },
            [&](IStore1)
            {
                context.setLocal(1, context.pop<std::int32_t>());
                return NextPC{};
            },
            [&](IStore2)
            {
                context.setLocal(2, context.pop<std::int32_t>());
                return NextPC{};
            },
            [&](IStore3)
            {
                context.setLocal(3, context.pop<std::int32_t>());
                return NextPC{};
            },
            [&](IStore iStore)
            {
                context.setLocal(iStore.index, context.pop<std::int32_t>());
                return NextPC{};
            },
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

// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Interpreter.hpp"

#include <jllvm/class/ByteCodeIterator.hpp>

#include "VirtualMachine.hpp"

std::uint64_t jllvm::Interpreter::executeMethod(const Method& method, std::uint16_t& offset,
                                                InterpreterContext& /*context*/)
{
    Code* code = method.getMethodInfo().getAttributes().find<Code>();
    llvm::ArrayRef<char> codeArray = code->getCode();
    auto curr = ByteCodeIterator(codeArray.drop_front(offset).data(), offset);

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
    };

    using InstructionResult = swl::variant<SetPC, NextPC, ReturnValue>;

    while (true)
    {
        // Update the current offset to the new instruction.
        offset = curr.getOffset();
        InstructionResult result =
            match(*curr,
                  [&](...) -> InstructionResult
                  {
                      // While the interpreter is not fully implemented, we escaped to JIT code that implements the
                      // given bytecode instruction.
                      // TODO: Remove this once interpreter implements all bytecodes.
                      m_virtualMachine.unwindJavaStack(
                          [&](JavaFrame frame) { m_virtualMachine.getJIT().doI2JOnStackReplacement(frame, offset); });
                      llvm_unreachable("not possible");
                  });

        if (auto* returnValue = get_if<ReturnValue>(&result))
        {
            return returnValue->value;
        }

        match(
            result, [](ReturnValue) {}, [&](NextPC) { ++curr; },
            [&](SetPC setPc) { curr = ByteCodeIterator(codeArray.drop_front(setPc.newPC).data(), setPc.newPC); });
    }
}

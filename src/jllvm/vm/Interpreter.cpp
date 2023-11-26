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

std::uint64_t jllvm::Interpreter::executeMethod(const Method& method, std::uint16_t& offset,
                                                InterpreterContext& /*context*/)
{
    Code* code = method.getMethodInfo().getAttributes().find<Code>();
    llvm::ArrayRef<char> codeArray = code->getCode();
    auto curr = ByteCodeIterator(codeArray.data(), offset);

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
            [&](SetPC setPc) { curr = ByteCodeIterator(codeArray.data(), setPc.newPC); });
    }
}

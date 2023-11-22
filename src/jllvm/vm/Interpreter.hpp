
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <jllvm/object/ClassObject.hpp>

#include <cstdint>

namespace jllvm
{

class VirtualMachine;

/// Context used in the execution of one Java frame. It incorporates and contains convenience methods for interacting
/// with local variables and the operand stack.
class InterpreterContext
{
    std::uint16_t& m_topOfStack;
    std::uint64_t* m_operandStack;
    std::uint64_t* m_operandGCMask;
    std::uint64_t* m_localVariables;
    std::uint64_t* m_localVariableGCMask;

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

    // TODO: Implement methods for operand stack and global variables.
};

/// Interpreter instance containing all global state of the interpreter.
class Interpreter
{
    VirtualMachine& m_virtualMachine;
    /// Enable OSR from the interpreter into the JIT if the method is hot enough.
    bool m_enableOSR;

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

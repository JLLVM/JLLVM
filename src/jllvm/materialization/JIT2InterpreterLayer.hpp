
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm/ExecutionEngine/Orc/Layer.h>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Layer used to compile stubs to start interpreting a method. The functions added to the layer use direct call name
/// mangling and adhere to the C calling convention used by VM code and the JIT.
class JIT2InterpreterLayer : public ByteCodeLayer
{
    llvm::orc::IRLayer& m_baseLayer;
    llvm::DataLayout m_dataLayout;

public:
    JIT2InterpreterLayer(llvm::orc::MangleAndInterner& mangler, llvm::orc::IRLayer& baseLayer,
                              const llvm::DataLayout& dataLayout)
        : ByteCodeLayer(mangler), m_baseLayer(baseLayer), m_dataLayout(dataLayout)
    {
    }

    void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const Method* method) override;
};
} // namespace jllvm

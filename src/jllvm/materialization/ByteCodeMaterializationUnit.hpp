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

#include <llvm/ExecutionEngine/Orc/Core.h>

#include "ByteCodeLayer.hpp"

namespace jllvm
{
/// Materialization unit to add a JVM Byte code method to the JITLink graph and materializing it once required.
class ByteCodeMaterializationUnit : public llvm::orc::MaterializationUnit
{
    ByteCodeLayer& m_layer;
    const Method* m_method;

public:
    /// Creates a materialization unit for the method 'methodInfo' of the class 'classFile'.
    /// Compilation is done using 'layer'.
    ByteCodeMaterializationUnit(ByteCodeLayer& layer, const Method* method);

    llvm::StringRef getName() const override
    {
        return "ByteCodeMaterializationUnit";
    }

    void materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> r) override;

private:
    void discard(const llvm::orc::JITDylib&, const llvm::orc::SymbolStringPtr&) override
    {
        llvm_unreachable("Should not be possible");
    }
};

} // namespace jllvm

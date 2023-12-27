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

#include "ByteCodeOSRLayer.hpp"

#include <jllvm/compiler/ClassObjectStubMangling.hpp>

namespace
{
using namespace jllvm;

class ByteCodeOSRMaterializationUnit : public llvm::orc::MaterializationUnit
{
    ByteCodeOSRLayer& m_layer;
    const jllvm::Method* m_method;
    std::uint16_t m_offset;
    CallingConvention m_callingConvention;

public:
    ByteCodeOSRMaterializationUnit(ByteCodeOSRLayer& layer, const jllvm::Method* method, std::uint16_t offset,
                                   CallingConvention callingConvention)
        : llvm::orc::MaterializationUnit(
              [&]
              {
                  llvm::orc::SymbolFlagsMap result;
                  std::string name = jllvm::mangleOSRMethod(method, offset);
                  result[layer.getInterner()(name)] = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
                  return llvm::orc::MaterializationUnit::Interface(std::move(result), nullptr);
              }()),
          m_layer(layer),
          m_method(method),
          m_offset(offset),
          m_callingConvention(callingConvention)
    {
    }

    llvm::StringRef getName() const override
    {
        return "ByteCodeOSRMaterializationUnit";
    }

    void materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> r) override
    {
        m_layer.emit(std::move(r), m_method, m_offset, m_callingConvention);
    }

private:
    void discard(const llvm::orc::JITDylib&, const llvm::orc::SymbolStringPtr&) override
    {
        llvm_unreachable("Should not be possible");
    }
};

} // namespace

llvm::Error jllvm::ByteCodeOSRLayer::add(llvm::orc::JITDylib& dylib, const jllvm::Method* method,
                                         std::uint16_t byteCodeOffset, CallingConvention callingConvention)
{
    return dylib.define(
        std::make_unique<ByteCodeOSRMaterializationUnit>(*this, method, byteCodeOffset, callingConvention));
}

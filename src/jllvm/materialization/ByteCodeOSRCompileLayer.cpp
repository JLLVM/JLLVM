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

#include "ByteCodeOSRCompileLayer.hpp"

#include "jllvm/compiler/Compiler.hpp"

void jllvm::ByteCodeOSRCompileLayer::emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr,
                                          const jllvm::Method* method, std::uint16_t offset)
{
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("name", *context);

    module->setDataLayout(m_dataLayout);
    module->setTargetTriple(LLVM_HOST_TRIPLE);

    compileOSRMethod(*module, offset, *method, m_stringInterner);

    m_baseLayer.emit(std::move(mr), llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

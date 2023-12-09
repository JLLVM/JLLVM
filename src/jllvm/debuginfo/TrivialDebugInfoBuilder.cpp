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

#include "TrivialDebugInfoBuilder.hpp"

#include <llvm/IR/Function.h>

jllvm::TrivialDebugInfoBuilder::TrivialDebugInfoBuilder(llvm::Function* function)
    : m_debugBuilder(*function->getParent())
{
    llvm::DIFile* file = m_debugBuilder.createFile(".", ".");
    unsigned runtimeVersion = 1;
    m_debugBuilder.createCompileUnit(llvm::dwarf::DW_LANG_Java, file, /*Producer=*/"JLLVM", /*isOptimized=*/true,
                                     /*Flags=*/"", runtimeVersion);

    m_subProgram = m_debugBuilder.createFunction(
        file, /*Name=*/function->getName(), /*LinkageName=*/function->getName(), file, /*LineNo=*/1,
        m_debugBuilder.createSubroutineType(m_debugBuilder.getOrCreateTypeArray({})), /*ScopeLine=*/1,
        /*Flags=*/llvm::DINode::FlagZero, /*SPFlags=*/llvm::DISubprogram::SPFlagDefinition);

    function->setSubprogram(m_subProgram);
}

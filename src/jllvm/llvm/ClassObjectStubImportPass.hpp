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

#include <llvm/IR/PassManager.h>

#include <jllvm/object/ClassLoader.hpp>

namespace jllvm
{

/// LLVM Optimization pass that imports the function definition of any calls to functions with mangling
/// from the 'ClassObjectStubMangling.hpp' file IFF the corresponding class object is already loaded.
/// The produced function definitions are marked with internal linkage and replaces the previous declarations.
/// This pass should be run as early in the optimization pipeline as possible to allow LLVM to inline the generated
/// definitions.
class ClassObjectStubImportPass : public llvm::PassInfoMixin<ClassObjectStubImportPass>
{
    ClassLoader& m_classLoader;

public:
    explicit ClassObjectStubImportPass(ClassLoader& classLoader) : m_classLoader(classLoader) {}

    /// Run function with signature indicating the pass manager that this is a module pass.
    llvm::PreservedAnalyses run(llvm::Module& module, llvm::ModuleAnalysisManager& analysisManager);
};
} // namespace jllvm

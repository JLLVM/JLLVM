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

#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/PassManager.h>

namespace jllvm
{
/// Convenience pass placed right after any sanitizer or similar instrumentation passes.
/// This pass does basically nothing but mark calls to instrumentation functions as "gc-leaf-functions" to improve
/// codegen of the output and avoid calls to these functions being converted to state points.
class MarkSanitizersGCLeafsPass : public llvm::PassInfoMixin<MarkSanitizersGCLeafsPass>
{
public:
    explicit MarkSanitizersGCLeafsPass() = default;

    /// Run function with signature indicating the pass manager that this is a module pass.
    llvm::PreservedAnalyses run(llvm::Module& M, llvm::ModuleAnalysisManager& AM);
};
} // namespace jllvm

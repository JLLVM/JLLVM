
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

#include <llvm/IR/DIBuilder.h>

namespace jllvm
{

/// Class used to build the bare minimum of useful debug info for a single function module.
/// It creates a 'DISubprogram' for a given function, using the symbol name of the function as name of the function.
class TrivialDebugInfoBuilder
{
    llvm::DIBuilder m_debugBuilder;
    llvm::DISubprogram* m_subProgram;

public:

    /// Constructs the builder and creates debug info for 'function'.
    TrivialDebugInfoBuilder(llvm::Function* function);

    ~TrivialDebugInfoBuilder()
    {
        finalize();
    }

    TrivialDebugInfoBuilder(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder(TrivialDebugInfoBuilder&&) = delete;
    TrivialDebugInfoBuilder& operator=(const TrivialDebugInfoBuilder&) = delete;
    TrivialDebugInfoBuilder& operator=(TrivialDebugInfoBuilder&&) = delete;

    /// Returns a noop debug info location for use by 'IRBuilder'.
    llvm::DILocation* getNoopLoc() const
    {
        return llvm::DILocation::get(m_subProgram->getContext(), 1, 1, m_subProgram);
    }

    /// Finalizes debug info. This method must be called at the end of constructing the LLVM module.
    /// This method is also called by the destructor.
    void finalize()
    {
        if (!m_subProgram)
        {
            return;
        }
        m_debugBuilder.finalizeSubprogram(std::exchange(m_subProgram, nullptr));
        m_debugBuilder.finalize();
    }
};

} // namespace jllvm

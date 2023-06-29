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

#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/StringSaver.h>

namespace jllvm
{
/// Class used to manage resources used during and for command line parsing.
class CommandLine : public llvm::opt::PrecomputedOptTable
{
    llvm::BumpPtrAllocator m_allocator;
    llvm::StringSaver m_stringSaver;
    llvm::opt::InputArgList m_args;

public:
    CommandLine(llvm::ArrayRef<char*> args);

    llvm::opt::InputArgList& getArgs()
    {
        return m_args;
    }
};

enum ID
{
    /// NOLINTNEXTLINE(readability-identifier-naming): Predefined name used by llvm-tblgen.
    OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, HELPTEXT, METAVAR, VALUES) OPT_##ID,
#include <jllvm/main/Opts.inc>
#undef OPTION
};

} // namespace jllvm

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

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>

#include <jllvm/main/Main.hpp>

int main(int argc, char** argv)
{
    llvm::InitLLVM initLlvm(argc, argv);

    auto executablePath = llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void*>(&main));
    return jllvm::main(executablePath, {argv, static_cast<std::size_t>(argc)});
}

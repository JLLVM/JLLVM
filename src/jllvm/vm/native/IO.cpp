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

#include "IO.hpp"

#include <llvm/Support/FileSystem.h>

void jllvm::io::FileDescriptorModel::close0()
{
    std::uint32_t& fd = state.fdField(javaThis);
    if (fd == -1)
    {
        return;
    }
    llvm::sys::fs::file_t native = llvm::sys::fs::convertFDToNativeFile(fd);
    (void)llvm::sys::fs::closeFile(native);
    fd = -1;
}

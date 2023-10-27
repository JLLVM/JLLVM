# Copyright (C) 2023 The JLLVM Contributors.
#
# This file is part of JLLVM.
#
# JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3, or (at your option) any later version.
#
# JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
# of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
# see <http://www.gnu.org/licenses/>.

include(TableGen)

set(LLVM_TARGET_DEFINITIONS Opts.td)
tablegen(LLVM Opts.inc -gen-opt-parser-defs)
add_public_tablegen_target(JLLVMJVMCOptsTableGen)

add_executable(jllvm-jvmc main.cpp)
target_link_libraries(jllvm-jvmc PRIVATE JLLVMCompiler LLVMOption)
add_dependencies(jllvm-jvmc JLLVMJVMCOptsTableGen)
target_include_directories(jllvm-jvmc PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

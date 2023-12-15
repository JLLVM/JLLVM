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
add_public_tablegen_target(JLLVMMainOptsTableGen)

llvm_map_components_to_libnames(llvm_native_libs ${LLVM_NATIVE_ARCH})

add_library(JLLVMMain Main.cpp CommandLine.cpp CommandLine.hpp)
target_link_libraries(JLLVMMain PUBLIC LLVMSupport PRIVATE ${llvm_native_libs} LLVMOption JLLVMVirtualMachine)
target_include_directories(JLLVMMain PUBLIC ${PROJECT_BINARY_DIR}/src)
add_dependencies(JLLVMMain JLLVMMainOptsTableGen)

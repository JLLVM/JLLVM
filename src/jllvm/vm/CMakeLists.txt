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

llvm_map_components_to_libnames(llvm_native_libs ${LLVM_NATIVE_ARCH})

add_library(JLLVMVirtualMachine VirtualMachine.cpp JIT.cpp StackMapRegistrationPlugin.cpp
        JNIImplementation.cpp NativeImplementation.cpp JavaFrame.cpp native/IO.cpp
        native/Lang.cpp native/JDK.cpp native/Security.cpp
        Interpreter.cpp
        Runtime.cpp
        JNIBridge.cpp
)
target_link_libraries(JLLVMVirtualMachine
        PRIVATE JLLVMLLVMPasses LLVMTargetParser LLVMOrcTargetProcess LLVMScalarOpts LLVMAnalysis ${llvm_native_libs}
        jni_headers
        PUBLIC JLLVMClassParser JLLVMObject JLLVMGC JLLVMMaterialization JLLVMUnwinder LLVMExecutionEngine LLVMOrcJIT
        LLVMJITLink LLVMOrcShared
        )

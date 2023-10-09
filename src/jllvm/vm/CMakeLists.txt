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
        JNIImplementation.cpp NativeImplementation.cpp MarkSanitizersGCLeafs.cpp native/IO.cpp
        native/Lang.cpp native/JDK.cpp native/Security.cpp
        ClassObjectStubImportPass.cpp)
target_link_libraries(JLLVMVirtualMachine
        PRIVATE JLLVMUnwinder ${llvm_native_libs} LLVMTargetParser LLVMOrcTargetProcess LLVMScalarOpts LLVMAnalysis
        PUBLIC JLLVMClassParser JLLVMObject JLLVMGC JLLVMMaterialization LLVMExecutionEngine LLVMOrcJIT LLVMJITLink
        LLVMPasses
        )

set(JAVA_INCLUDE_PATH "${JAVA_HOME}/../include")
find_path(JNI_INCLUDE_PATH1 jni.h REQUIRED PATHS ${JAVA_INCLUDE_PATH})
find_path(JNI_INCLUDE_PATH2 NAMES jni_md.h jniport.h REQUIRED
        PATHS ${JAVA_INCLUDE_PATH}
        ${JAVA_INCLUDE_PATH}/darwin
        ${JAVA_INCLUDE_PATH}/win32
        ${JAVA_INCLUDE_PATH}/linux
        ${JAVA_INCLUDE_PATH}/freebsd
        ${JAVA_INCLUDE_PATH}/openbsd
        ${JAVA_INCLUDE_PATH}/solaris
        ${JAVA_INCLUDE_PATH}/hp-ux
        ${JAVA_INCLUDE_PATH}/alpha
        ${JAVA_INCLUDE_PATH}/aix
        )

target_include_directories(JLLVMVirtualMachine PRIVATE ${JNI_INCLUDE_PATH1} ${JNI_INCLUDE_PATH2})

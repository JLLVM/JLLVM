
llvm_map_components_to_libnames(llvm_native_libs ${LLVM_NATIVE_ARCH})

add_library(JLLVMVirtualMachine VirtualMachine.cpp JIT.cpp GarbageCollector.cpp StackMapRegistrationPlugin.cpp
        JNIImplementation.cpp)
target_link_libraries(JLLVMVirtualMachine
        PRIVATE ${llvm_native_libs}
        PUBLIC JLLVMClassParser JLLVMObject LLVMExecutionEngine LLVMOrcJIT LLVMJITLink JLLVMMaterialization
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

set_source_files_properties(JNIImplementation.cpp PROPERTIES INCLUDE_DIRECTORIES "${JNI_INCLUDE_PATH1};${JNI_INCLUDE_PATH2}")

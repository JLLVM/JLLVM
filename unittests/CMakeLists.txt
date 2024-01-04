CPMAddPackage("gh:catchorg/Catch2@3.3.2")

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(GCTests RootFreeListTests.cpp
        GarbageCollectorTests.cpp
        GCRootRefTests.cpp)
target_link_libraries(GCTests JLLVMGC Catch2::Catch2WithMain)
catch_discover_tests(GCTests)

add_executable(SupportTests NonOwningFrozenSetTests.cpp
        BitArrayRefTests.cpp)
target_link_libraries(SupportTests JLLVMSupport Catch2::Catch2WithMain)
catch_discover_tests(SupportTests)

add_executable(ClassTests DescriptorTests.cpp)
target_link_libraries(ClassTests JLLVMClassParser Catch2::Catch2WithMain)
catch_discover_tests(ClassTests)

set(class_files)

# Compiles 'source_file' in 'Inputs' to a class file. The source file is expected to contain contain a public class
# with the same name as the source file. Producing a class file with the expected name is impossible otherwise.
macro(compile_java_test_file source_file)
    set(arg_source_file "${CMAKE_CURRENT_SOURCE_DIR}/Inputs/${source_file}")

    set(out_file ${source_file})
    cmake_path(REPLACE_EXTENSION out_file LAST_ONLY class)
    cmake_path(GET out_file FILENAME out_file)
    set(out_file ${CMAKE_CURRENT_BINARY_DIR}/${out_file})
    list(APPEND class_files ${out_file})
    cmake_path(RELATIVE_PATH out_file BASE_DIRECTORY ${CMAKE_BINARY_DIR} OUTPUT_VARIABLE out_file_message)
    add_custom_command(
            OUTPUT ${out_file}
            COMMAND ${Java_JAVAC_EXECUTABLE}
            -d ${CMAKE_CURRENT_BINARY_DIR}
            -implicit:none
            -encoding utf-8
            ${arg_source_file}
            COMMENT "Building JAVA object ${out_file_message}"
            DEPENDS ${arg_source_file}
    )
endmacro()

macro(compile_java_test_files)
    foreach (file ${ARGN})
        compile_java_test_file(${file})
    endforeach ()
endmacro()

compile_java_test_files(
        TestSimpleJNI.java
)

add_custom_target(jni-java-compile DEPENDS ${class_files})

add_executable(JNITests JNITests.cpp)
target_link_libraries(JNITests JLLVMVirtualMachine Catch2::Catch2WithMain jni_headers)
target_compile_definitions(JNITests PRIVATE
        "JAVA_BASE_PATH=\"${CMAKE_BINARY_DIR}/lib/java.base\""
        "INPUTS_BASE_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\"")
catch_discover_tests(JNITests)
add_dependencies(JNITests jni-java-compile)

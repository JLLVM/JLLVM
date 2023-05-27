cmake_minimum_required(VERSION 3.20)
project(JLLVM)

set(CMAKE_CXX_STANDARD 20)

if( NOT uppercase_CMAKE_BUILD_TYPE STREQUAL "DEBUG" )
    option(JLLVM_ENABLE_ASSERTIONS "Enable assertions" OFF)
else()
    option(JLLVM_ENABLE_ASSERTIONS "Enable assertions" ON)
endif()

include(cmake/CPM.cmake)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)
find_package(LLVM REQUIRED CONFIG)

find_package(Java 17 COMPONENTS Development REQUIRED)

get_filename_component(REAL_JAVAC ${Java_JAVAC_EXECUTABLE} REALPATH)
cmake_path(GET REAL_JAVAC PARENT_PATH JAVA_HOME)
if (NOT EXISTS ${CMAKE_BINARY_DIR}/lib)
    execute_process(COMMAND ${JAVA_HOME}/jimage extract --dir=${CMAKE_BINARY_DIR}/lib ${JAVA_HOME}/../lib/modules)
endif ()

message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT LLVM_ENABLE_RTTI)
    if (MSVC)
        string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
    else ()
        string(REGEX REPLACE "-frtti" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
    endif ()
endif ()

if (JLLVM_ENABLE_ASSERTIONS)
    # On non-Debug builds cmake automatically defines NDEBUG, so we
    # explicitly undefine it:
    if( NOT uppercase_CMAKE_BUILD_TYPE STREQUAL "DEBUG" )
        # NOTE: use `add_compile_options` rather than `add_definitions` since
        # `add_definitions` does not support generator expressions.
        add_compile_options($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-UNDEBUG>)
        if (MSVC)
            # Also remove /D NDEBUG to avoid MSVC warnings about conflicting defines.
            foreach (flags_var_to_scrub
                    CMAKE_CXX_FLAGS_RELEASE
                    CMAKE_CXX_FLAGS_RELWITHDEBINFO
                    CMAKE_CXX_FLAGS_MINSIZEREL
                    CMAKE_C_FLAGS_RELEASE
                    CMAKE_C_FLAGS_RELWITHDEBINFO
                    CMAKE_C_FLAGS_MINSIZEREL)
                string (REGEX REPLACE "(^| )[/-]D *NDEBUG($| )" " "
                        "${flags_var_to_scrub}" "${${flags_var_to_scrub}}")
            endforeach()
        endif()
    endif()
endif ()

set(JLLVM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(JLLVM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(JLLVM_TOOLS_DIR ${CMAKE_BINARY_DIR}/bin)

include_directories(src)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(SYSTEM 3rdParty)
link_directories(${LLVM_BUILD_LIBRARY_DIR})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(CheckCXXCompilerFlag)

# Matching LLVMs visibility option here. Mismatch of visibility can cause linker warnings on macOS.
if ((NOT (${CMAKE_SYSTEM_NAME} MATCHES "AIX")) AND
(NOT (WIN32 OR CYGWIN) OR (MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")))
    # GCC for MinGW does nothing about -fvisibility-inlines-hidden, but warns
    # about use of the attributes. As long as we don't use the attributes (to
    # override the default) we shouldn't set the command line options either.
    # GCC on AIX warns if -fvisibility-inlines-hidden is used and Clang on AIX doesn't currently support visibility.
    check_cxx_compiler_flag("-fvisibility-inlines-hidden" SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
    if (SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility-inlines-hidden")
    endif ()
endif ()

add_subdirectory(src)
add_subdirectory(tools)

include(CTest)
enable_testing()
add_subdirectory(tests)
add_subdirectory(unittests)

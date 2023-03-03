cmake_minimum_required(VERSION 3.20)
project(JLLVM)

set(CMAKE_CXX_STANDARD 17)

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

set(JLLVM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(JLLVM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(JLLVM_TOOLS_DIR ${CMAKE_BINARY_DIR}/bin)

include_directories(src)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_BUILD_LIBRARY_DIR})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(src)
add_subdirectory(tools)

include(CTest)
enable_testing()
add_subdirectory(tests)

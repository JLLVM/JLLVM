
set(LLVM_TABLEGEN_EXE "llvm-tblgen")

include(TableGen)

set(LLVM_TARGET_DEFINITIONS Opts.td)
tablegen(LLVM Opts.inc -gen-opt-parser-defs)
add_public_tablegen_target(JLLVMMainOptsTableGen)

add_library(JLLVMMain Main.cpp CommandLine.cpp CommandLine.hpp)
target_link_libraries(JLLVMMain PUBLIC LLVMSupport PRIVATE LLVMOption JLLVMVirtualMachine)
target_include_directories(JLLVMMain PUBLIC ${PROJECT_BINARY_DIR}/src)
add_dependencies(JLLVMMain JLLVMMainOptsTableGen)

add_library(JLLVMClassParser ClassFile.cpp Descriptors.cpp Descriptors.hpp)
target_link_libraries(JLLVMClassParser PUBLIC LLVMSupport)

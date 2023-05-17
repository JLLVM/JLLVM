add_library(JLLVMSupport Bytes.cpp Encoding.cpp)
target_link_libraries(JLLVMSupport PUBLIC LLVMSupport)

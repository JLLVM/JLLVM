add_library(JLLVMSupport Bytes.cpp Encoding.cpp FileUtils.cpp)
target_link_libraries(JLLVMSupport PUBLIC LLVMSupport)

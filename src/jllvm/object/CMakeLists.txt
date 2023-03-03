
add_library(JLLVMObject ClassLoader.cpp ClassObject.cpp)
target_link_libraries(JLLVMObject PUBLIC JLLVMClassParser)

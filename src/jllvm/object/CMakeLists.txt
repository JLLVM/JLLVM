
add_library(JLLVMObject ClassLoader.cpp ClassObject.cpp Object.cpp)
target_link_libraries(JLLVMObject PUBLIC JLLVMClassParser)

add_library(JLLVMClassParser ClassFile.cpp Descriptors.cpp Descriptors.hpp
        ByteCodeIterator.cpp
        ByteCodeIterator.hpp)
target_link_libraries(JLLVMClassParser PUBLIC JLLVMSupport)

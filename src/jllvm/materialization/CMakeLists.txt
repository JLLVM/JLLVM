add_library(JLLVMMaterialization
        ByteCodeCompileLayer.cpp
        ByteCodeLayer.cpp
        ByteCodeMaterializationUnit.cpp
        ByteCodeOnDemandLayer.cpp
        JNIImplementationLayer.cpp
        ByteCodeCompileUtils.cpp ByteCodeCompileUtils.hpp)
target_link_libraries(JLLVMMaterialization PUBLIC JLLVMVirtualMachine JLLVMObject LLVMCore LLVMOrcJIT)

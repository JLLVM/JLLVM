
add_library(JLLVMMaterialization
        ByteCodeCompileLayer.cpp
        ByteCodeLayer.cpp
        ByteCodeMaterializationUnit.cpp
        ByteCodeOnDemandLayer.cpp
        JNIImplementationLayer.cpp
        LambdaMaterialization.cpp ByteCodeCompileUtils.cpp ByteCodeCompileUtils.hpp)
target_link_libraries(JLLVMMaterialization PUBLIC JLLVMObject LLVMCore LLVMOrcJIT)

add_library(JLLVMMaterialization
        ByteCodeCompileLayer.cpp
        ByteCodeCompileUtils.cpp
        ByteCodeLayer.cpp
        ByteCodeMaterializationUnit.cpp
        ByteCodeOnDemandLayer.cpp
        JNIImplementationLayer.cpp
        CodeGenerator.cpp
        CodeGeneratorUtils.cpp)
target_link_libraries(JLLVMMaterialization PUBLIC JLLVMVirtualMachine JLLVMObject LLVMCore LLVMOrcJIT)

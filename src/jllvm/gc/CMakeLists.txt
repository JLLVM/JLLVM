add_library(JLLVMGC GarbageCollector.cpp RootFreeList.cpp)
target_link_libraries(JLLVMGC PUBLIC JLLVMObject PRIVATE JLLVMUnwinder)

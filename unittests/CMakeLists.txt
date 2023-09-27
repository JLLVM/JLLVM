CPMAddPackage("gh:catchorg/Catch2@3.3.2")

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(GCTests RootFreeListTests.cpp
        GarbageCollectorTests.cpp
        GCRootRefTests.cpp)
target_link_libraries(GCTests JLLVMGC Catch2::Catch2WithMain)
catch_discover_tests(GCTests)

add_executable(SupportTests NonOwningFrozenSetTests.cpp)
target_link_libraries(SupportTests JLLVMSupport Catch2::Catch2WithMain)
catch_discover_tests(SupportTests)

add_executable(ClassTests DescriptorTests.cpp)
target_link_libraries(ClassTests JLLVMClassParser Catch2::Catch2WithMain)
catch_discover_tests(ClassTests)

find_package(Python3 3.6 COMPONENTS Interpreter REQUIRED)

set(JLLVM_TEST_DEPENDS
        FileCheck count not split-file
        jllvm
        )

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
        @ONLY
)

add_test(NAME lit-tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/jllvm-lit.py "${CMAKE_CURRENT_BINARY_DIR}" -v)
set_tests_properties(lit-tests PROPERTIES TIMEOUT 3600)

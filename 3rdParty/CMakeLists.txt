
enable_language(ASM)
set(LIBUNWIND_ENABLE_ASSERTIONS ${JLLVM_ENABLE_ASSERTIONS})
set(LIBUNWIND_ENABLE_SHARED OFF)
set(LIBUNWIND_ENABLE_FRAME_APIS ON)
set(LIBUNWIND_HIDE_SYMBOLS ON)
add_subdirectory(libunwind EXCLUDE_FROM_ALL SYSTEM)

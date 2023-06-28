#include "IO.hpp"

#include <llvm/Support/FileSystem.h>

void jllvm::io::FileDescriptorModel::close0()
{
    if (javaThis->fd == -1)
    {
        return;
    }
    llvm::sys::fs::file_t native = llvm::sys::fs::convertFDToNativeFile(javaThis->fd);
    (void)llvm::sys::fs::closeFile(native);
    javaThis->fd = -1;
}

#include "FileUtils.hpp"

#ifdef __unix__
    #include <fcntl.h>

bool jllvm::isAppendMode(int fd)
{
    return fcntl(fd, F_GETFL) & O_APPEND;
}

#endif

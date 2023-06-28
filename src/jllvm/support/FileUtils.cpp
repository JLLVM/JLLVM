#include "FileUtils.hpp"

#if defined(__unix__) || defined(__APPLE__)
    #include <fcntl.h>

bool jllvm::isAppendMode(int fd)
{
    return fcntl(fd, F_GETFL) & O_APPEND;
}

#endif

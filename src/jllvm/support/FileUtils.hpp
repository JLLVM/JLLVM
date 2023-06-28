#pragma once

namespace jllvm
{
/// Returns true if the opened file referred to by 'fd' was opened in append mode.
bool isAppendMode(int fd);
} // namespace jllvm

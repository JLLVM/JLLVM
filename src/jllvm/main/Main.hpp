#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

namespace jllvm
{
/// Main program of JLLVM. 'executablePath' is the absolute path to the executable and 'args' are the arguments
/// INCLUDING the executable name/path.
int main(llvm::StringRef executablePath, llvm::ArrayRef<char*> args);
} // namespace jllvm

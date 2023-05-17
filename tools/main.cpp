#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>

#include <jllvm/main/Main.hpp>

int main(int argc, char** argv)
{
    llvm::InitLLVM initLlvm(argc, argv);

    auto executablePath = llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void*>(&main));
    return jllvm::main(executablePath, {argv, static_cast<std::size_t>(argc)});
}

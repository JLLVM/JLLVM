#include "MarkSanitizersGCLeafs.hpp"

llvm::PreservedAnalyses jllvm::MarkSanitizersGCLeafsPass::run(llvm::Module& M, llvm::ModuleAnalysisManager&)
{
    constexpr llvm::StringRef instrumentationPrefixes[] = {"__asan_", "__tsan_"};

    for (llvm::Function& function : M.getFunctionList())
    {
        llvm::StringRef name = function.getName();
        if (llvm::none_of(instrumentationPrefixes, [&](llvm::StringRef prefix) { return name.starts_with(prefix); }))
        {
            continue;
        }
        function.addFnAttr("gc-leaf-function");
    }
    return llvm::PreservedAnalyses::all();
}

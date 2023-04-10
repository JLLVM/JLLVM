#include "Main.hpp"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include "CommandLine.hpp"

namespace
{
template <class T>
auto trivialPrintFunction()
{
    return llvm::JITEvaluatedSymbol::fromPointer(+[](void*, void*, T value)
                                                 { llvm::outs() << static_cast<std::ptrdiff_t>(value) << '\n'; });
}

} // namespace

int jllvm::main(llvm::StringRef executablePath, llvm::ArrayRef<char*> args)
{
    llvm::SmallVector<const char*> llvmArgs{"jllvm"};
#ifndef __APPLE__
    // libunwind (from LLVM), seemingly does not properly write to caller saved registers. We therefore disable this
    // optimization.
    llvmArgs.push_back("-fixup-allow-gcptr-in-csr");
    llvmArgs.push_back("-max-registers-for-gc-values=1000");
#endif

#ifndef NDEBUG
    llvmArgs.push_back("-jllvm-gc-every-alloc=1");
#endif
    llvm::cl::ParseCommandLineOptions(llvmArgs.size(), llvmArgs.data());

    CommandLine commandLine(args);

    auto inputFiles = commandLine.getArgs().getAllArgValues(OPT_INPUT);
    if (inputFiles.empty())
    {
        llvm::report_fatal_error("Expected one input file");
    }

    llvm::SmallString<64> modulesPath = llvm::sys::path::parent_path(llvm::sys::path::parent_path(executablePath));
    llvm::sys::path::append(modulesPath, "lib");

    std::vector<std::string> classPath;
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator iter(modulesPath, ec), end; iter != end && !ec; iter = iter.increment(ec))
    {
        classPath.push_back(iter->path());
    }

    llvm::SmallString<32> inputFile(inputFiles.front());
    llvm::sys::fs::make_absolute(inputFile);
    classPath.emplace_back(llvm::sys::path::parent_path(inputFile));

    for (llvm::StringRef paths : commandLine.getArgs().getAllArgValues(OPT_classpath))
    {
        llvm::SmallVector<llvm::StringRef> splits;
        paths.split(splits, ';', -1, /*KeepEmpty=*/false);
        llvm::append_range(classPath, splits);
    }

    jllvm::VirtualMachine vm(std::move(classPath));

    if (commandLine.getArgs().hasArg(OPT_Xenable_test_utils))
    {
        vm.addJNISymbols(llvm::orc::absoluteSymbols({
            {vm.getInterner()("Java_Test_print__B"), trivialPrintFunction<std::int8_t>()},
            {vm.getInterner()("Java_Test_print__D"), trivialPrintFunction<double>()},
            {vm.getInterner()("Java_Test_print__F"), trivialPrintFunction<float>()},
            {vm.getInterner()("Java_Test_print__I"), trivialPrintFunction<std::int32_t>()},
            {vm.getInterner()("Java_Test_print__J"), trivialPrintFunction<std::int64_t>()},
            {vm.getInterner()("Java_Test_print__S"), trivialPrintFunction<std::int16_t>()},
            {vm.getInterner()("Java_Test_print__C"), trivialPrintFunction<std::uint16_t>()},
            {vm.getInterner()("Java_Test_print__Z"), trivialPrintFunction<bool>()},
        }));
    }

    return vm.executeMain(inputFiles.front(), llvm::to_vector_of<llvm::StringRef>(llvm::drop_begin(inputFiles)));
}

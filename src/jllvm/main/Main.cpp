#include "Main.hpp"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include <sstream>

#include "CommandLine.hpp"

namespace
{
template <class T, class = void>
struct TrivialPrinter
{
    auto operator()(void*, void*, T value)
    {
        llvm::outs() << static_cast<std::ptrdiff_t>(value) << '\n';
    }
};

template <class T>
struct TrivialPrinter<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
    auto operator()(void*, void*, T value)
    {
        std::stringstream str;
        str << std::defaultfloat << value << '\n';
        llvm::outs() << str.str();
    }
};

template <>
struct TrivialPrinter<jllvm::String>
{
    auto operator()(void*, void*, jllvm::String* string)
    {
        llvm::outs() << string->toUTF8() << '\n';
    }
};

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

    CommandLine commandLine(args);

#ifndef NDEBUG
    llvm::SmallString<64> buffer;
    llvmArgs.push_back("-jllvm-gc-every-alloc=1");
    if (llvm::StringRef value = commandLine.getArgs().getLastArgValue(OPT_Xdebug_EQ); !value.empty())
    {
        llvmArgs.push_back(("-debug-only=" + value).toNullTerminatedStringRef(buffer).data());
    }
#endif
    llvm::cl::ParseCommandLineOptions(llvmArgs.size(), llvmArgs.data());

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
        JIT& jit = vm.getJIT();
        jit.addJNISymbol("Java_Test_print__B", TrivialPrinter<std::int8_t>{});
        jit.addJNISymbol("Java_Test_print__D", TrivialPrinter<double>{});
        jit.addJNISymbol("Java_Test_print__F", TrivialPrinter<float>{});
        jit.addJNISymbol("Java_Test_print__I", TrivialPrinter<std::int32_t>{});
        jit.addJNISymbol("Java_Test_print__J", TrivialPrinter<std::int64_t>{});
        jit.addJNISymbol("Java_Test_print__S", TrivialPrinter<std::int16_t>{});
        jit.addJNISymbol("Java_Test_print__C", TrivialPrinter<std::uint16_t>{});
        jit.addJNISymbol("Java_Test_print__Z", TrivialPrinter<bool>{});
        jit.addJNISymbol("Java_Test_print__Ljava/lang/String;", TrivialPrinter<String>{});
    }

    return vm.executeMain(inputFiles.front(), llvm::to_vector_of<llvm::StringRef>(llvm::drop_begin(inputFiles)));
}

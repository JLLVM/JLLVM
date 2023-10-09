// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include "Main.hpp"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include <iomanip>
#include <sstream>

#include "CommandLine.hpp"

namespace
{
template <class T>
struct TrivialPrinter
{
    auto operator()(void*, void*, T value)
    {
        llvm::outs() << static_cast<std::ptrdiff_t>(value) << '\n';
    }
};

template <class T>
requires(std::is_floating_point_v<T>) struct TrivialPrinter<T>
{
    auto operator()(void*, void*, T value)
    {
        std::stringstream str;
        str << std::setprecision(std::numeric_limits<T>::digits10) << value << '\n';
        llvm::outs() << str.str();
    }
};

template <>
struct TrivialPrinter<jllvm::String>
{
    auto operator()(void*, void*, jllvm::GCRootRef<jllvm::String> string)
    {
        llvm::outs() << string->toUTF8() << '\n';
    }
};

} // namespace

int jllvm::main(llvm::StringRef executablePath, llvm::ArrayRef<char*> args)
{
    llvm::SmallVector<const char*> llvmArgs{"jllvm"};
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
    llvm::SmallString<64> javaHome = modulesPath;
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

    BootOptions bootOptions{
        .javaHome = javaHome,
        .classPath = std::move(classPath),
        .systemInitialization = commandLine.getArgs().hasFlag(OPT_Xsystem_init, OPT_Xno_system_init, true),
    };

    jllvm::VirtualMachine vm(std::move(bootOptions));

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

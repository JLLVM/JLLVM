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

#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <jllvm/compiler/Compiler.hpp>
#include <jllvm/object/ClassLoader.hpp>

#include <iterator>

enum ID
{
    /// NOLINTNEXTLINE(readability-identifier-naming): Predefined name used by llvm-tblgen.
    OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, HELPTEXT, METAVAR, VALUES) OPT_##ID,
#include "Opts.inc"
#undef OPTION
};

#define PREFIX(NAME, VALUE)                                     \
    static constexpr llvm::StringLiteral NAME##_init[] = VALUE; \
    static constexpr llvm::ArrayRef<llvm::StringLiteral> NAME(NAME##_init, std::size(NAME##_init) - 1);
#include "Opts.inc"
#undef PREFIX

static constexpr llvm::opt::OptTable::Info INFO_TABLE[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, HELPTEXT, METAVAR, VALUES) \
    {PREFIX, NAME,  HELPTEXT,    METAVAR,     OPT_##ID,  llvm::opt::Option::KIND##Class,                 \
     PARAM,  FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS, VALUES},
#include "Opts.inc"
#undef OPTION
};

namespace
{
class OptTable : public llvm::opt::GenericOptTable
{
public:
    OptTable(llvm::ArrayRef<Info> optionInfos) : GenericOptTable(optionInfos) {}
};
} // namespace

int main(int argc, char** argv)
{
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver stringSaver(allocator);
    OptTable optTable(INFO_TABLE);

    bool error = false;
    llvm::opt::InputArgList args = optTable.parseArgs(argc, argv, OPT_UNKNOWN, stringSaver,
                                                      [&](llvm::StringRef errorMessage)
                                                      {
                                                          llvm::errs() << errorMessage;
                                                          error = true;
                                                      });
    if (error)
    {
        return -1;
    }

    if (args.hasArg(OPT_help))
    {
        optTable.printHelp(llvm::outs(), "jllvm-jvmc [opts] --method <name>:<descriptor> class-file", "jllvm-jvmc");
        return 0;
    }

    if (args.getAllArgValues(OPT_method).size() != 1)
    {
        llvm::errs() << "expected exactly one occurrence of '--method'\n";
        return -1;
    }

    llvm::StringRef value = args.getLastArgValue(OPT_method);
    auto [name, descriptor] = value.split(':');
    if (descriptor.empty())
    {
        llvm::errs() << "expected method in format '<name>:<descriptor>'\n";
        return -1;
    }
    if (!jllvm::MethodType::verify(descriptor))
    {
        llvm::errs() << "invalid method descriptor '" << descriptor << "'\n";
        return -1;
    }
    jllvm::MethodType methodType(descriptor);

    if (args.getAllArgValues(OPT_INPUT).size() != 1)
    {
        llvm::errs() << "expected exactly one input class file\n";
        return -1;
    }

    // Add to the class path the development class files extracted by the cmake build from the found JDK.
    std::string executablePath = llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void*>(&main));
    llvm::SmallString<64> modulesPath = llvm::sys::path::parent_path(llvm::sys::path::parent_path(executablePath));
    llvm::sys::path::append(modulesPath, "lib");

    std::vector<std::string> classPath;
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator iter(modulesPath, ec), end; iter != end && !ec; iter = iter.increment(ec))
    {
        classPath.push_back(iter->path());
    }

    // Also add to the class path the directory of the input file to be able to load any classes in the same directory.
    llvm::SmallString<32> inputFile(args.getLastArgValue(OPT_INPUT));
    llvm::sys::fs::make_absolute(inputFile);
    classPath.emplace_back(llvm::sys::path::parent_path(inputFile));

    jllvm::StringInterner stringInterner;

    jllvm::ClassLoader loader(
        stringInterner, std::move(classPath), [](jllvm::ClassObject&) {},
        [&]() -> void** { return new (allocator.Allocate<void*>()) void* {}; });

    loader.loadBootstrapClasses();

    auto buffer = llvm::MemoryBuffer::getFile(inputFile);
    if (!buffer)
    {
        llvm::errs() << "failed to open " << inputFile << '\n';
        return -1;
    }

    jllvm::ClassObject& classObject = loader.add(std::move(*buffer));

    const jllvm::Method* method = classObject.getMethod(name, methodType);
    if (!method)
    {
        llvm::errs() << "failed to find method '" << name << ":" << methodType.textual() << "' in '"
                     << classObject.getClassName() << "'\n";
        return -1;
    }

    llvm::LLVMContext context;
    llvm::Module module(name, context);

    if (llvm::opt::Arg* arg = args.getLastArg(OPT_osr))
    {
        llvm::StringRef ref = arg->getValue();
        unsigned offset;
        if (ref.consumeInteger(0, offset))
        {
            llvm::errs() << "invalid integer '" << ref << "' as argument to '--osr'\n";
            return -1;
        }
        compileOSRMethod(module, offset, *method);
    }
    else
    {
        compileMethod(module, *method);
    }
    if (llvm::verifyModule(module, &llvm::dbgs()))
    {
        std::abort();
    }

    module.print(llvm::outs(), nullptr);
}

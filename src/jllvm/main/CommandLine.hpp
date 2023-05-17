#pragma once

#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/StringSaver.h>

namespace jllvm
{
/// Class used to manage resources used during and for command line parsing.
class CommandLine : public llvm::opt::PrecomputedOptTable
{
    llvm::BumpPtrAllocator m_allocator;
    llvm::StringSaver m_stringSaver;
    llvm::opt::InputArgList m_args;

public:
    CommandLine(llvm::ArrayRef<char*> args);

    llvm::opt::InputArgList& getArgs()
    {
        return m_args;
    }
};

enum ID
{
    /// NOLINTNEXTLINE(readability-identifier-naming): Predefined name used by llvm-tblgen.
    OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, HELPTEXT, METAVAR, VALUES) OPT_##ID,
#include <jllvm/main/Opts.inc>
#undef OPTION
};

} // namespace jllvm

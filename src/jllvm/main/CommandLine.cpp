#include "CommandLine.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Option/Option.h>

#include <iterator>

#define PREFIX(NAME, VALUE)                                     \
    static constexpr llvm::StringLiteral NAME##_init[] = VALUE; \
    static constexpr llvm::ArrayRef<llvm::StringLiteral> NAME(NAME##_init, std::size(NAME##_init) - 1);
#include <jllvm/main/Opts.inc>
#undef PREFIX

static constexpr const llvm::StringLiteral PREFIX_TABLE_INIT[] =
#define PREFIX_UNION(VALUES) VALUES
#include <jllvm/main/Opts.inc>
#undef PREFIX_UNION
    ;
static constexpr const llvm::ArrayRef<llvm::StringLiteral> PREFIX_TABLE(PREFIX_TABLE_INIT,
                                                                        std::size(PREFIX_TABLE_INIT) - 1);

// Don't have much choice until this is fixed in LLVM
using llvm::opt::HelpHidden;

static constexpr llvm::opt::OptTable::Info INFO_TABLE[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, HELPTEXT, METAVAR, VALUES) \
    {PREFIX,                                                                                             \
     NAME,                                                                                               \
     HELPTEXT,                                                                                           \
     METAVAR,                                                                                            \
     ::jllvm::OPT_##ID,                                                                                  \
     llvm::opt::Option::KIND##Class,                                                                     \
     PARAM,                                                                                              \
     FLAGS,                                                                                              \
     ::jllvm::OPT_##GROUP,                                                                               \
     ::jllvm::OPT_##ALIAS,                                                                               \
     ALIASARGS,                                                                                          \
     VALUES},
#include <jllvm/main/Opts.inc>
#undef OPTION
};

jllvm::CommandLine::CommandLine(llvm::ArrayRef<char*> args)
    : llvm::opt::PrecomputedOptTable{INFO_TABLE, PREFIX_TABLE},
      m_stringSaver{m_allocator},
      m_args{parseArgs(args.size(), args.data(), OPT_UNKNOWN, m_stringSaver,
                       [](llvm::StringRef error) { llvm::report_fatal_error(error); })}
{
}

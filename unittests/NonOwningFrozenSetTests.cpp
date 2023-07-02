#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

#include <llvm/ADT/StringRef.h>

#include <jllvm/support/NonOwningFrozenSet.hpp>

using namespace jllvm;
using namespace Catch::Matchers;

TEST_CASE("Construction", "[NonOwningFrozenSet]")
{
    llvm::BumpPtrAllocator allocator;
    std::vector<std::size_t> v = {3, 5, 7};

    NonOwningFrozenSet set(v, allocator);

    CHECK_THAT(set, RangeEquals(v));
    CHECK_FALSE(set.empty());

    CHECK(set.find(2) == set.end());
    CHECK(set.find(3) != set.end());

    STATIC_REQUIRE(std::is_standard_layout_v<decltype(set)>);
}

namespace
{
struct Thing
{
    llvm::StringRef name;
    std::size_t data;

    bool operator==(const Thing&) const = default;

    bool operator==(llvm::StringRef other) const
    {
        return name == other;
    }
};

llvm::hash_code hash_value(const Thing& thing)
{
    return hash_value(thing.name);
}

} // namespace

TEST_CASE("Heterogeneous lookup", "[NonOwningFrozenSet]")
{
    llvm::BumpPtrAllocator allocator;
    std::vector<Thing> v = {{"Hello", 3}, {"World", 5}, {"!", 7}};

    NonOwningFrozenSet set(v, allocator);

    CHECK_THAT(set, RangeEquals(v));
    CHECK_FALSE(set.empty());

    CHECK(set.find(llvm::StringRef{"..."}) == set.end());

    const auto* lookup = set.find(llvm::StringRef{"Hello"});
    REQUIRE(lookup != set.end());
    CHECK(lookup->data == 3);

    lookup = set.find(llvm::StringRef{"World"});
    REQUIRE(lookup != set.end());
    CHECK(lookup->data == 5);

    lookup = set.find(llvm::StringRef{"!"});
    REQUIRE(lookup != set.end());
    CHECK(lookup->data == 7);
}

TEST_CASE("Empty Set", "[NonOwningFrozenSet]")
{
    llvm::BumpPtrAllocator allocator;
    std::vector<std::size_t> v;

    NonOwningFrozenSet set(v, allocator);

    CHECK_THAT(set, RangeEquals(v));

    CHECK(set.empty());
    CHECK(set.find(2) == set.end());
    CHECK(set.find(3) == set.end());
}

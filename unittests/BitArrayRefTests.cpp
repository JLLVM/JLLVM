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

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_quantifiers.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

#include <jllvm/support/BitArrayRef.hpp>

using namespace jllvm;
using namespace Catch::Matchers;

TEMPLATE_PRODUCT_TEST_CASE("BitArrayRef", "[BitArrayRef]", (BitArrayRef, MutableBitArrayRef),
                           (std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t))
{
    typename TestType::value_type value{};
    TestType ref(&value, /*size=*/5);
    CHECK(ref.size() == 5);
    CHECK(std::distance(ref.begin(), ref.end()) == 5);
    CHECK_THAT(ref, NoneTrue());

    value = 0b11111;
    CHECK_THAT(ref, AllTrue());

    value = 0b01001;
    CHECK_THAT(ref, RangeEquals(std::initializer_list<bool>{true, false, false, true, false}));
    CHECK(ref[0] == true);
    CHECK(ref[1] == false);
    CHECK(ref[2] == false);
    CHECK(ref[3] == true);
    CHECK(ref[4] == false);

    CHECK_THAT(llvm::make_range(ref.words_begin(), ref.words_end()),
               RangeEquals(std::initializer_list<decltype(value)>{value}));

    int size = std::numeric_limits<typename TestType::value_type>::digits;
    ref = TestType(&value, size);
    CHECK(ref[size - 1] == false);
}

TEMPLATE_TEST_CASE("MutableBitArrayRef", "[MutableBitArrayRef]", std::uint8_t, std::uint16_t, std::uint32_t,
                   std::uint64_t)
{
    TestType value{};
    MutableBitArrayRef ref(&value, /*size=*/5);
    CHECK(std::distance(ref.begin(), ref.end()) == 5);

    ref[0] = true;
    ref[1] = true;
    ref[2] = true;
    ref[3] = true;
    ref[4] = true;
    CHECK_THAT(ref, AllTrue());

    ref[0] = true;
    ref[1] = false;
    ref[2] = false;
    ref[3] = true;
    ref[4] = false;
    CHECK_THAT(ref, RangeEquals(std::initializer_list<bool>{true, false, false, true, false}));
    CHECK(ref[0] == true);
    CHECK(ref[1] == false);
    CHECK(ref[2] == false);
    CHECK(ref[3] == true);
    CHECK(ref[4] == false);
}

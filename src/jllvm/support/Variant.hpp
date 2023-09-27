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

#pragma once

#include <utility>

namespace jllvm
{

namespace detail
{
template <class... Ts>
struct Overload : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
Overload(Ts...) -> Overload<Ts...>;

} // namespace detail

/// Convenience function for matching on the contained types of a variant.
/// 'matchers' must be a list of lambdas or other callable classes which may accept one or more alternatives of the
/// variant. 'matchers' may also return a value which is then returned by 'match' as long as all return types are of
/// a common base type.
/// This function failed to compile if none of 'matchers' can be called with an alternative within the variant.
/// Note: You can use a '[](auto){}' lambda as a 'catch-all' else case.
template <typename Variant, typename... Matchers>
constexpr decltype(auto) match(Variant&& variant, Matchers&&... matchers)
{
    if constexpr (requires { variant.valueless_by_exception(); })
    {
        assert(!variant.valueless_by_exception());
    }
    return visit(detail::Overload{std::forward<Matchers>(matchers)...}, std::forward<Variant>(variant));
}
} // namespace jllvm


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
    assert(!variant.valueless_by_exception());
    return visit(detail::Overload{std::forward<Matchers>(matchers)...}, std::forward<Variant>(variant));
}
} // namespace jllvm


#pragma once
#include <llvm/Support/Compiler.h>

#include <variant>

namespace jllvm
{
template <typename T, typename Variant>
constexpr decltype(auto) get(Variant&& variant) noexcept
{
    assert(!variant.valueless_by_exception() && std::holds_alternative<T>(variant));
    auto* value = std::get_if<T>(&variant);
    assert(value);
    if constexpr (std::is_lvalue_reference_v<Variant>)
    {
        return *value;
    }
    else
    {
        return std::move(*value);
    }
}

template <std::size_t i, typename Variant>
constexpr decltype(auto) get(Variant&& variant) noexcept
{
    assert(!variant.valueless_by_exception() && variant.index() == i);
    auto* value = std::get_if<i>(&variant);
    assert(value);
    if constexpr (std::is_lvalue_reference_v<Variant>)
    {
        return *value;
    }
    else
    {
        return std::move(*value);
    }
}

namespace detail
{
template <class... Ts>
struct Overload : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
Overload(Ts...) -> Overload<Ts...>;

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant,
                                   std::enable_if_t<(i == 0 || i > 12)>* = nullptr)
{
    if (variant.index() == i)
    {
        return std::forward<Callable>(callable)(jllvm::get<i>(std::forward<Variant>(variant)));
    }
    if constexpr (i != 0)
    {
        return visitImpl<i - 1>(std::forward<Callable>(callable), std::forward<Variant>(variant));
    }
    else
    {
        llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 1)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 2)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 3)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 4)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 5)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 6)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 7)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 8)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        case 8: return std::forward<Callable>(callable)(jllvm::get<8>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 9)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        case 8: return std::forward<Callable>(callable)(jllvm::get<8>(std::forward<Variant>(variant)));
        case 9: return std::forward<Callable>(callable)(jllvm::get<9>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 10)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        case 8: return std::forward<Callable>(callable)(jllvm::get<8>(std::forward<Variant>(variant)));
        case 9: return std::forward<Callable>(callable)(jllvm::get<9>(std::forward<Variant>(variant)));
        case 10: return std::forward<Callable>(callable)(jllvm::get<10>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 11)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        case 8: return std::forward<Callable>(callable)(jllvm::get<8>(std::forward<Variant>(variant)));
        case 9: return std::forward<Callable>(callable)(jllvm::get<9>(std::forward<Variant>(variant)));
        case 10: return std::forward<Callable>(callable)(jllvm::get<10>(std::forward<Variant>(variant)));
        case 11: return std::forward<Callable>(callable)(jllvm::get<11>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <std::size_t i, class Callable, class Variant>
constexpr decltype(auto) visitImpl(Callable&& callable, Variant&& variant, std::enable_if_t<(i == 12)>* = nullptr)
{
    switch (variant.index())
    {
        case 0: return std::forward<Callable>(callable)(jllvm::get<0>(std::forward<Variant>(variant)));
        case 1: return std::forward<Callable>(callable)(jllvm::get<1>(std::forward<Variant>(variant)));
        case 2: return std::forward<Callable>(callable)(jllvm::get<2>(std::forward<Variant>(variant)));
        case 3: return std::forward<Callable>(callable)(jllvm::get<3>(std::forward<Variant>(variant)));
        case 4: return std::forward<Callable>(callable)(jllvm::get<4>(std::forward<Variant>(variant)));
        case 5: return std::forward<Callable>(callable)(jllvm::get<5>(std::forward<Variant>(variant)));
        case 6: return std::forward<Callable>(callable)(jllvm::get<6>(std::forward<Variant>(variant)));
        case 7: return std::forward<Callable>(callable)(jllvm::get<7>(std::forward<Variant>(variant)));
        case 8: return std::forward<Callable>(callable)(jllvm::get<8>(std::forward<Variant>(variant)));
        case 9: return std::forward<Callable>(callable)(jllvm::get<9>(std::forward<Variant>(variant)));
        case 10: return std::forward<Callable>(callable)(jllvm::get<10>(std::forward<Variant>(variant)));
        case 11: return std::forward<Callable>(callable)(jllvm::get<11>(std::forward<Variant>(variant)));
        case 12: return std::forward<Callable>(callable)(jllvm::get<12>(std::forward<Variant>(variant)));
        default: llvm_unreachable("Invalid index");
    }
}

template <class Callable, class Variant>
constexpr decltype(auto) visit(Callable&& callable, Variant&& variant)
{
    assert(!variant.valueless_by_exception());
    return visitImpl<std::variant_size_v<std::decay_t<Variant>> - 1>(std::forward<Callable>(callable),
                                                                     std::forward<Variant>(variant));
}
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
    return detail::visit(detail::Overload{std::forward<Matchers>(matchers)...}, std::forward<Variant>(variant));
}
} // namespace jllvm

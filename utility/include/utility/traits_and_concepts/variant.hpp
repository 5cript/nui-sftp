#pragma once

#include <variant>
#include <type_traits>

namespace Utility
{
    template <typename... Ts>
    struct IsVariant : std::false_type
    {};

    template <typename... Ts>
    struct IsVariant<std::variant<Ts...>> : std::true_type
    {};

    template <typename T>
    constexpr auto IsVariant_v = IsVariant<T>::value;

    template <typename T>
    concept VariantType = IsVariant_v<T>;
}
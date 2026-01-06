#pragma once

#include <utility/describe.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <string>
#include <stdexcept>

namespace Utility
{
    template <typename EnumType>
    std::string enumToString(EnumType const& enumValue)
    {
        char const* result = nullptr;
        boost::mp11::mp_for_each<boost::describe::describe_enumerators<EnumType>>(
            [&result, &enumValue](auto desc)
            {
                if (enumValue == desc.value)
                    result = desc.name;
            }
        );

        if (result == nullptr)
            throw std::invalid_argument("Invalid enum value");
        return result;
    }

    template <typename EnumType>
    EnumType enumFromString(std::string const& str)
    {
        const auto lower = Algorithm::toLowerCase(str);
        EnumType enumValue;
        bool found = false;
        boost::mp11::mp_for_each<boost::describe::describe_enumerators<EnumType>>(
            [&found, &enumValue, &lower](auto desc)
            {
                if (lower == Algorithm::toLowerCase(desc.name))
                {
                    enumValue = desc.value;
                    found = true;
                }
            }
        );

        if (!found)
            throw std::invalid_argument("Invalid enum string: " + str);

        return enumValue;
    }
}
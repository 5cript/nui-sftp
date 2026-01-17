#pragma once

#include <optional>

auto nulloptReset(auto& member, auto onChange)
{
    return [&member, onChange = std::move(onChange)]()
    {
        member.value(std::nullopt);
        onChange();
    };
}

auto valueReset(auto& member, auto onChange, auto value)
{
    return [&member, onChange = std::move(onChange), value = std::move(value)]()
    {
        member.value(value);
        onChange();
    };
}
#pragma once

#include <fmt/format.h>
#include <gimo_ext/StdOptional.hpp>
#include <gimo.hpp>

#include <string>
#include <optional>

namespace Utility
{
    enum class OrderOfMagnitude
    {
        None = 0,
        Kilo,
        Mega,
        Giga,
        Tera
    };

    inline OrderOfMagnitude determineOrderOfMagnitude(long long value)
    {
        if (value < 1000)
            return OrderOfMagnitude::None;
        else if (value < 1'000'000)
            return OrderOfMagnitude::Kilo;
        else if (value < 1'000'000'000)
            return OrderOfMagnitude::Mega;
        else if (value < 1'000'000'000'000)
            return OrderOfMagnitude::Giga;
        else
            return OrderOfMagnitude::Tera;
    }

    inline std::string formatBytes(long long value, std::optional<OrderOfMagnitude> const& magnitudeOpt = std::nullopt)
    {
        const auto magnitude = *gimo::apply(
            magnitudeOpt,
            gimo::or_else(
                [&value]()
                {
                    return std::optional{determineOrderOfMagnitude(value)};
                }
            )
        );

        switch (magnitude)
        {
            case OrderOfMagnitude::None:
                return fmt::format("{} B", value);
            case OrderOfMagnitude::Kilo:
                return fmt::format("{:.2f} KB", value / 1024.0);
            case OrderOfMagnitude::Mega:
                return fmt::format("{:.2f} MB", value / (1024.0 * 1024.0));
            case OrderOfMagnitude::Giga:
                return fmt::format("{:.2f} GB", value / (1024.0 * 1024.0 * 1024.0));
            case OrderOfMagnitude::Tera:
                return fmt::format("{:.2f} TB", value / (1024.0 * 1024.0 * 1024.0 * 1024.0));
        }
        return std::string{};
    }
}
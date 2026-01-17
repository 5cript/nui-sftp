#include <frontend/color.hpp>
#include <nui/frontend/val.hpp>

#include <fmt/format.h>

std::string Color::toPoundSignRGBA() const
{
    if (alpha_ == 255)
    {
        return fmt::format(
            "#{:02X}{:02X}{:02X}",
            static_cast<unsigned int>(red_),
            static_cast<unsigned int>(green_),
            static_cast<unsigned int>(blue_)
        );
    }
    return fmt::format(
        "#{:02X}{:02X}{:02X}{:02X}",
        static_cast<unsigned int>(red_),
        static_cast<unsigned int>(green_),
        static_cast<unsigned int>(blue_),
        static_cast<unsigned int>(alpha_)
    );
}

std::optional<Color> parseCssColor(std::string const& cssColor)
{
    auto rgba = Nui::val::global("colorStringToRGBAObject")(cssColor);
    if (rgba.isNull() || rgba.isUndefined())
        return std::nullopt;
    std::uint8_t r = static_cast<std::uint8_t>(rgba["r"].as<int>());
    std::uint8_t g = static_cast<std::uint8_t>(rgba["g"].as<int>());
    std::uint8_t b = static_cast<std::uint8_t>(rgba["b"].as<int>());
    std::uint8_t a = static_cast<std::uint8_t>(rgba["a"].as<int>());
    return Color{r, g, b, a};
}
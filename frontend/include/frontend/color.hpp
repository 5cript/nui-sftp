#pragma once

#include <string>
#include <optional>
#include <cstdint>

class Color
{
  public:
    Color(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255)
        : red_{red}
        , green_{green}
        , blue_{blue}
        , alpha_{alpha}
    {}

    std::uint8_t red() const
    {
        return red_;
    }
    void red(std::uint8_t red)
    {
        red_ = red;
    }
    std::uint8_t green() const
    {
        return green_;
    }
    void green(std::uint8_t green)
    {
        green_ = green;
    }
    std::uint8_t blue() const
    {
        return blue_;
    }
    void blue(std::uint8_t blue)
    {
        blue_ = blue;
    }
    std::uint8_t alpha() const
    {
        return alpha_;
    }
    void alpha(std::uint8_t alpha)
    {
        alpha_ = alpha;
    }

    std::string toPoundSignRGBA() const;

  private:
    std::uint8_t red_;
    std::uint8_t green_;
    std::uint8_t blue_;
    std::uint8_t alpha_;
};

std::optional<Color> parseCssColor(std::string const& cssColor);
#pragma once

#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/color_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/text_setting.hpp>

struct TerminalOptions
{
    struct TerminalTheme
    {
        ColorSetting<true> background;
        ColorSetting<true> black;
        ColorSetting<true> blue;
        ColorSetting<true> brightBlack;
        ColorSetting<true> brightBlue;
        ColorSetting<true> brightCyan;
        ColorSetting<true> brightGreen;
        ColorSetting<true> brightMagenta;
        ColorSetting<true> brightRed;
        ColorSetting<true> brightWhite;
        ColorSetting<true> brightYellow;
        ColorSetting<true> cursor;
        ColorSetting<true> cursorAccent;
        ColorSetting<true> cyan;
        ColorSetting<true> foreground;
        ColorSetting<true> green;
        ColorSetting<true> magenta;
        ColorSetting<true> red;
        ColorSetting<true> selectionBackground;
        ColorSetting<true> selectionForeground;
        ColorSetting<true> selectionInactiveBackground;
        ColorSetting<true> white;
        ColorSetting<true> yellow;

        // TODO:
        // std::optional<std::vector<std::string>> extendedAnsi{std::nullopt};

        TerminalTheme(std::function<void()> const& onChange);
    };

    TextSetting<true> fontFamily;
    NumberSetting<unsigned int, true> fontSize;
    NumberSetting<float, true> lineHeight;
    BoolSetting<true> cursorBlink;
    TextSetting<true> renderer;
    NumberSetting<int, true> letterSpacing;
    TerminalTheme theme;

    Nui::Observed<std::string> groupKey{"default"};
    Nui::Observed<std::vector<std::string>> groupKeys{{"default"}};
    Nui::Observed<bool> themeEngaged;

    TerminalOptions(std::function<void()> const& onChange);
};
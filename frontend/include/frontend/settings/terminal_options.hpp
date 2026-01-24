#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/color_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/terminal_options.hpp>

struct TerminalOptions : public GroupKeys
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

        TerminalTheme(std::function<void()> const& onChange, Nui::Observed<bool>* externalEngage);
    };

    TextSetting<true> fontFamily;
    NumberSetting<unsigned int, true> fontSize;
    NumberSetting<float, true> lineHeight;
    BoolSetting<true> cursorBlink;
    ComboSetting<std::string, std::string, true> renderer;
    NumberSetting<int, true> letterSpacing;
    Nui::Observed<bool> themeEngaged;
    TerminalTheme theme;

    TerminalOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::TerminalOptions& state) const;
    void loadFromState(Persistence::TerminalOptions const& state);
    void assumeDefaultsFrom(Persistence::TerminalOptions const& state);
    Nui::ElementRenderer render();

  private:
    std::function<void()> onChange_;
};
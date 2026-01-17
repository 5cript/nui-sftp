#include <frontend/settings/terminal_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>

using namespace std::string_literals;

TerminalOptions::TerminalTheme::TerminalTheme(std::function<void()> const& onChange)
    : background{
            language->getObserved("settings", "terminalOptions", "theme", "backgroundHelpText"),
            onChange,
            valueReset(background, onChange, "#202020"s),
        }
    , black{
            language->getObserved("settings", "terminalOptions", "theme", "blackHelpText"),
            onChange,
            valueReset(black, onChange, "#000000"s),
        }
    , blue{
            language->getObserved("settings", "terminalOptions", "theme", "blueHelpText"),
            onChange,
            valueReset(blue, onChange, "#0000FF"s),
        }
    , brightBlack{
            language->getObserved("settings", "terminalOptions", "theme", "brightBlackHelpText"),
            onChange,
            valueReset(brightBlack, onChange, "#555555"s),
        }
    , brightBlue{
            language->getObserved("settings", "terminalOptions", "theme", "brightBlueHelpText"),
            onChange,
            valueReset(brightBlue, onChange, "#5555FF"s),
        }
    , brightCyan{
            language->getObserved("settings", "terminalOptions", "theme", "brightCyanHelpText"),
            onChange,
            valueReset(brightCyan, onChange, "#55FFFF"s),
        }
    , brightGreen{
            language->getObserved("settings", "terminalOptions", "theme", "brightGreenHelpText"),
            onChange,
            valueReset(brightGreen, onChange, "#55FF55"s),
        }
    , brightMagenta{
            language->getObserved("settings", "terminalOptions", "theme", "brightMagentaHelpText"),
            onChange,
            valueReset(brightMagenta, onChange, "#FF55FF"s),
        }
    , brightRed{
            language->getObserved("settings", "terminalOptions", "theme", "brightRedHelpText"),
            onChange,
            valueReset(brightRed, onChange, "#FF5555"s),
        }
    , brightWhite{
            language->getObserved("settings", "terminalOptions", "theme", "brightWhiteHelpText"),
            onChange,
            valueReset(brightWhite, onChange, "#FFFFFF"s),
        }
    , brightYellow{
            language->getObserved("settings", "terminalOptions", "theme", "brightYellowHelpText"),
            onChange,
            valueReset(brightYellow, onChange, "#FFFF55"s),
        }
    , cursor{
            language->getObserved("settings", "terminalOptions", "theme", "cursorHelpText"),
            onChange,
            valueReset(cursor, onChange, "#FFFFFF"s),
        }
    , cursorAccent{
            language->getObserved("settings", "terminalOptions", "theme", "cursorAccentHelpText"),
            onChange,
            valueReset(cursorAccent, onChange, "#FFFFFF"s),
        }
    , cyan{
            language->getObserved("settings", "terminalOptions", "theme", "cyanHelpText"),
            onChange,
            valueReset(cyan, onChange, "#00FFFF"s),
        }
    , foreground{
            language->getObserved("settings", "terminalOptions", "theme", "foregroundHelpText"),
            onChange,
            valueReset(foreground, onChange, "#FFFFFF"s),
        }
    , green{
            language->getObserved("settings", "terminalOptions", "theme", "greenHelpText"),
            onChange,
            valueReset(green, onChange, "#00FF00"s),
        }
    , magenta{
            language->getObserved("settings", "terminalOptions", "theme", "magentaHelpText"),
            onChange,
            valueReset(magenta, onChange, "#FF00FF"s),
        }
    , red{
            language->getObserved("settings", "terminalOptions", "theme", "redHelpText"),
            onChange,
            valueReset(red, onChange, "#FF0000"s),
        }
    , selectionBackground{
            language->getObserved(
                "settings",
                "terminalOptions",
                "theme",
                "selectionBackgroundHelpText"
            ),
            onChange,
            valueReset(selectionBackground, onChange, "#FFFFFF"s),
        }
    , selectionForeground{
            language->getObserved(
                "settings",
                "terminalOptions",
                "theme",
                "selectionForegroundHelpText"
            ),
            onChange,
            valueReset(selectionForeground, onChange, "#FFFFFF"s),
        }
    , selectionInactiveBackground{
            language->getObserved(
                "settings",
                "terminalOptions",
                "theme",
                "selectionInactiveBackgroundHelpText"
            ),
            onChange,
            valueReset(selectionInactiveBackground, onChange, "#FFFFFF"s),
        }
    , white{
            language->getObserved("settings", "terminalOptions", "theme", "whiteHelpText"),
            onChange,
            valueReset(white, onChange, "#FFFFFF"s),
        }
    , yellow{
            language->getObserved("settings", "terminalOptions", "theme", "yellowHelpText"),
            onChange,
            valueReset(yellow, onChange, "#FFFF00"s),
        }
{}

TerminalOptions::TerminalOptions(std::function<void()> const& onChange)
    : fontFamily{
            language->getObserved("settings", "terminalOptions", "fontFamilyHelpText"),
            onChange,
            valueReset(fontFamily, onChange, "consolas, courier-new, courier, monospace"s),
        }
    , fontSize{
            language->getObserved("settings", "terminalOptions", "fontSizeHelpText"),
            onChange,
            valueReset(fontSize, onChange, 14),
        }
    , lineHeight{
            language->getObserved("settings", "terminalOptions", "lineHeightHelpText"),
            onChange,
            valueReset(lineHeight, onChange, 1.0),
        }
    , cursorBlink{
            language->getObserved("settings", "terminalOptions", "cursorBlinkHelpText"),
            onChange,
            valueReset(cursorBlink, onChange, false),
        }
    , renderer{
            language->getObserved("settings", "terminalOptions", "rendererHelpText"),
            onChange,
            valueReset(renderer, onChange, "canvas"s),
        }
    , letterSpacing{
            language->getObserved("settings", "terminalOptions", "letterSpacingHelpText"),
            onChange,
            valueReset(letterSpacing, onChange, 0),
        }
    , theme{onChange}
{}
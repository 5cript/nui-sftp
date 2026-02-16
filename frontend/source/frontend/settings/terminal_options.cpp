#include <frontend/settings/terminal_options.hpp>
#include <frontend/settings/setting_helper.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/subgroup.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

using namespace std::string_literals;

TerminalOptions::TerminalTheme::TerminalTheme(std::function<void()> const& onChange, Nui::Observed<bool>* externalEngage)
    : background{
            language->getObserved("settings", "terminalOptions", "theme", "backgroundHelpText"),
            onChange,
            valueReset(background, onChange, "#202020"s),
            externalEngage
        }
    , black{
            language->getObserved("settings", "terminalOptions", "theme", "blackHelpText"),
            onChange,
            valueReset(black, onChange, "#000000"s),
            externalEngage
    }
    , blue{
            language->getObserved("settings", "terminalOptions", "theme", "blueHelpText"),
            onChange,
            valueReset(blue, onChange, "#0000FF"s),
            externalEngage
        }
    , brightBlack{
            language->getObserved("settings", "terminalOptions", "theme", "brightBlackHelpText"),
            onChange,
            valueReset(brightBlack, onChange, "#555555"s),
            externalEngage
        }
    , brightBlue{
            language->getObserved("settings", "terminalOptions", "theme", "brightBlueHelpText"),
            onChange,
            valueReset(brightBlue, onChange, "#5555FF"s),
            externalEngage
        }
    , brightCyan{
            language->getObserved("settings", "terminalOptions", "theme", "brightCyanHelpText"),
            onChange,
            valueReset(brightCyan, onChange, "#55FFFF"s),
            externalEngage
        }
    , brightGreen{
            language->getObserved("settings", "terminalOptions", "theme", "brightGreenHelpText"),
            onChange,
            valueReset(brightGreen, onChange, "#55FF55"s),
            externalEngage
        }
    , brightMagenta{
            language->getObserved("settings", "terminalOptions", "theme", "brightMagentaHelpText"),
            onChange,
            valueReset(brightMagenta, onChange, "#FF55FF"s),
            externalEngage
        }
    , brightRed{
            language->getObserved("settings", "terminalOptions", "theme", "brightRedHelpText"),
            onChange,
            valueReset(brightRed, onChange, "#FF5555"s),
            externalEngage
        }
    , brightWhite{
            language->getObserved("settings", "terminalOptions", "theme", "brightWhiteHelpText"),
            onChange,
            valueReset(brightWhite, onChange, "#FFFFFF"s),
            externalEngage
        }
    , brightYellow{
            language->getObserved("settings", "terminalOptions", "theme", "brightYellowHelpText"),
            onChange,
            valueReset(brightYellow, onChange, "#FFFF55"s),
            externalEngage
        }
    , cursor{
            language->getObserved("settings", "terminalOptions", "theme", "cursorHelpText"),
            onChange,
            valueReset(cursor, onChange, "#FFFFFF"s),
            externalEngage
        }
    , cursorAccent{
            language->getObserved("settings", "terminalOptions", "theme", "cursorAccentHelpText"),
            onChange,
            valueReset(cursorAccent, onChange, "#FFFFFF"s),
            externalEngage
        }
    , cyan{
            language->getObserved("settings", "terminalOptions", "theme", "cyanHelpText"),
            onChange,
            valueReset(cyan, onChange, "#00FFFF"s),
            externalEngage
        }
    , foreground{
            language->getObserved("settings", "terminalOptions", "theme", "foregroundHelpText"),
            onChange,
            valueReset(foreground, onChange, "#FFFFFF"s),
            externalEngage
        }
    , green{
            language->getObserved("settings", "terminalOptions", "theme", "greenHelpText"),
            onChange,
            valueReset(green, onChange, "#00FF00"s),
            externalEngage
        }
    , magenta{
            language->getObserved("settings", "terminalOptions", "theme", "magentaHelpText"),
            onChange,
            valueReset(magenta, onChange, "#FF00FF"s),
            externalEngage
        }
    , red{
            language->getObserved("settings", "terminalOptions", "theme", "redHelpText"),
            onChange,
            valueReset(red, onChange, "#FF0000"s),
            externalEngage
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
            externalEngage
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
            externalEngage
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
            externalEngage
        }
    , white{
            language->getObserved("settings", "terminalOptions", "theme", "whiteHelpText"),
            onChange,
            valueReset(white, onChange, "#FFFFFF"s),
            externalEngage
        }
    , yellow{
            language->getObserved("settings", "terminalOptions", "theme", "yellowHelpText"),
            onChange,
            valueReset(yellow, onChange, "#FFFF00"s),
            externalEngage
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
            {
                .minValue = 6,
                .maxValue = 1152,
            }
        }
    , lineHeight{
            language->getObserved("settings", "terminalOptions", "lineHeightHelpText"),
            onChange,
            valueReset(lineHeight, onChange, 1.0),
            {
                .minValue = 0.,
                .maxValue = 20.0,
                .stepValue = 0.1,
            }
        }
    , cursorBlink{
            language->getObserved("settings", "terminalOptions", "cursorBlinkHelpText"),
            onChange,
            valueReset(cursorBlink, onChange, false),
        }
    , renderer{
            std::vector<std::string>{"dom", "webgl"},
            language->getObserved("settings", "terminalOptions", "rendererHelpText"),
            onChange,
            valueReset(renderer, onChange, "webgl"s),
        }
    , letterSpacing{
            language->getObserved("settings", "terminalOptions", "letterSpacingHelpText"),
            onChange,
            valueReset(letterSpacing, onChange, 0),
            {
                .minValue = -1.0,
                .maxValue = 10.0,
                .stepValue = 0.1,
            }
        }
    , theme{onChange, &themeEngaged}
    , onChange_{onChange}
{}

void TerminalOptions::applyToState(Persistence::TerminalOptions& state) const
{
    assignIfValid(state.fontFamily, fontFamily);
    assignIfValid(state.fontSize, fontSize);
    assignIfValid(state.lineHeight, lineHeight);
    assignIfValid(state.cursorBlink, cursorBlink);
    assignIfValid(state.renderer, renderer);
    assignIfValid(state.letterSpacing, letterSpacing);
    if (themeEngaged.value())
    {
        state.theme = Persistence::TerminalTheme{
            .background = theme.background.valueIsValid() ? theme.background.value() : std::nullopt,
            .black = theme.black.valueIsValid() ? theme.black.value() : std::nullopt,
            .blue = theme.blue.valueIsValid() ? theme.blue.value() : std::nullopt,
            .brightBlack = theme.brightBlack.valueIsValid() ? theme.brightBlack.value() : std::nullopt,
            .brightBlue = theme.brightBlue.valueIsValid() ? theme.brightBlue.value() : std::nullopt,
            .brightCyan = theme.brightCyan.valueIsValid() ? theme.brightCyan.value() : std::nullopt,
            .brightGreen = theme.brightGreen.valueIsValid() ? theme.brightGreen.value() : std::nullopt,
            .brightMagenta = theme.brightMagenta.valueIsValid() ? theme.brightMagenta.value() : std::nullopt,
            .brightRed = theme.brightRed.valueIsValid() ? theme.brightRed.value() : std::nullopt,
            .brightWhite = theme.brightWhite.valueIsValid() ? theme.brightWhite.value() : std::nullopt,
            .brightYellow = theme.brightYellow.valueIsValid() ? theme.brightYellow.value() : std::nullopt,
            .cursor = theme.cursor.valueIsValid() ? theme.cursor.value() : std::nullopt,
            .cursorAccent = theme.cursorAccent.valueIsValid() ? theme.cursorAccent.value() : std::nullopt,
            .cyan = theme.cyan.valueIsValid() ? theme.cyan.value() : std::nullopt,
            .foreground = theme.foreground.valueIsValid() ? theme.foreground.value() : std::nullopt,
            .green = theme.green.valueIsValid() ? theme.green.value() : std::nullopt,
            .magenta = theme.magenta.valueIsValid() ? theme.magenta.value() : std::nullopt,
            .red = theme.red.valueIsValid() ? theme.red.value() : std::nullopt,
            .selectionBackground =
                theme.selectionBackground.valueIsValid() ? theme.selectionBackground.value() : std::nullopt,
            .selectionForeground =
                theme.selectionForeground.valueIsValid() ? theme.selectionForeground.value() : std::nullopt,
            .selectionInactiveBackground = theme.selectionInactiveBackground.valueIsValid()
                ? theme.selectionInactiveBackground.value()
                : std::nullopt,
            .white = theme.white.valueIsValid() ? theme.white.value() : std::nullopt,
            .yellow = theme.yellow.valueIsValid() ? theme.yellow.value() : std::nullopt,
        };
    }
    else
    {
        state.theme = std::nullopt;
    }
}

void TerminalOptions::loadFromState(Persistence::TerminalOptions const& state)
{
    fontFamily.value(state.fontFamily);
    fontSize.value(state.fontSize);
    lineHeight.value(state.lineHeight);
    cursorBlink.value(state.cursorBlink);
    renderer.value(state.renderer);
    letterSpacing.value(state.letterSpacing);

    themeEngaged = state.theme.has_value();
    if (state.theme.has_value())
    {
        const auto& themeState = state.theme.value();
        theme.background.value(themeState.background);
        theme.black.value(themeState.black);
        theme.blue.value(themeState.blue);
        theme.brightBlack.value(themeState.brightBlack);
        theme.brightBlue.value(themeState.brightBlue);
        theme.brightCyan.value(themeState.brightCyan);
        theme.brightGreen.value(themeState.brightGreen);
        theme.brightMagenta.value(themeState.brightMagenta);
        theme.brightRed.value(themeState.brightRed);
        theme.brightWhite.value(themeState.brightWhite);
        theme.brightYellow.value(themeState.brightYellow);
        theme.cursor.value(themeState.cursor);
        theme.cursorAccent.value(themeState.cursorAccent);
        theme.cyan.value(themeState.cyan);
        theme.foreground.value(themeState.foreground);
        theme.green.value(themeState.green);
        theme.magenta.value(themeState.magenta);
        theme.red.value(themeState.red);
        theme.selectionBackground.value(themeState.selectionBackground);
        theme.selectionForeground.value(themeState.selectionForeground);
        theme.selectionInactiveBackground.value(themeState.selectionInactiveBackground);
        theme.white.value(themeState.white);
        theme.yellow.value(themeState.yellow);
    }
    else
    {
        // All defaults:
        theme.background.value(std::nullopt);
        theme.black.value(std::nullopt);
        theme.blue.value(std::nullopt);
        theme.brightBlack.value(std::nullopt);
        theme.brightBlue.value(std::nullopt);
        theme.brightCyan.value(std::nullopt);
        theme.brightGreen.value(std::nullopt);
        theme.brightMagenta.value(std::nullopt);
        theme.brightRed.value(std::nullopt);
        theme.brightWhite.value(std::nullopt);
        theme.brightYellow.value(std::nullopt);
        theme.cursor.value(std::nullopt);
        theme.cursorAccent.value(std::nullopt);
        theme.cyan.value(std::nullopt);
        theme.foreground.value(std::nullopt);
        theme.green.value(std::nullopt);
        theme.magenta.value(std::nullopt);
        theme.red.value(std::nullopt);
        theme.selectionBackground.value(std::nullopt);
        theme.selectionForeground.value(std::nullopt);
        theme.selectionInactiveBackground.value(std::nullopt);
        theme.white.value(std::nullopt);
        theme.yellow.value(std::nullopt);
    }
}

void TerminalOptions::assumeDefaultsFrom(Persistence::TerminalOptions const& state)
{
    fontFamily.inherit(state.fontFamily);
    fontSize.inherit(state.fontSize);
    lineHeight.inherit(state.lineHeight);
    cursorBlink.inherit(state.cursorBlink);
    renderer.inherit(state.renderer);
    letterSpacing.inherit(state.letterSpacing);

    if (state.theme)
    {
        theme.background.inherit(state.theme->background);
        theme.black.inherit(state.theme->black);
        theme.blue.inherit(state.theme->blue);
        theme.brightBlack.inherit(state.theme->brightBlack);
        theme.brightBlue.inherit(state.theme->brightBlue);
        theme.brightCyan.inherit(state.theme->brightCyan);
        theme.brightGreen.inherit(state.theme->brightGreen);
        theme.brightMagenta.inherit(state.theme->brightMagenta);
        theme.brightRed.inherit(state.theme->brightRed);
        theme.brightWhite.inherit(state.theme->brightWhite);
        theme.brightYellow.inherit(state.theme->brightYellow);
        theme.cursor.inherit(state.theme->cursor);
        theme.cursorAccent.inherit(state.theme->cursorAccent);
        theme.cyan.inherit(state.theme->cyan);
        theme.foreground.inherit(state.theme->foreground);
        theme.green.inherit(state.theme->green);
        theme.magenta.inherit(state.theme->magenta);
        theme.red.inherit(state.theme->red);
        theme.selectionBackground.inherit(state.theme->selectionBackground);
        theme.selectionForeground.inherit(state.theme->selectionForeground);
        theme.selectionInactiveBackground.inherit(state.theme->selectionInactiveBackground);
        theme.white.inherit(state.theme->white);
        theme.yellow.inherit(state.theme->yellow);
    }
    else
    {
        theme.background.inherit(std::nullopt);
        theme.black.inherit(std::nullopt);
        theme.blue.inherit(std::nullopt);
        theme.brightBlack.inherit(std::nullopt);
        theme.brightBlue.inherit(std::nullopt);
        theme.brightCyan.inherit(std::nullopt);
        theme.brightGreen.inherit(std::nullopt);
        theme.brightMagenta.inherit(std::nullopt);
        theme.brightRed.inherit(std::nullopt);
        theme.brightWhite.inherit(std::nullopt);
        theme.brightYellow.inherit(std::nullopt);
        theme.cursor.inherit(std::nullopt);
        theme.cursorAccent.inherit(std::nullopt);
        theme.cyan.inherit(std::nullopt);
        theme.foreground.inherit(std::nullopt);
        theme.green.inherit(std::nullopt);
        theme.magenta.inherit(std::nullopt);
        theme.red.inherit(std::nullopt);
        theme.selectionBackground.inherit(std::nullopt);
        theme.selectionForeground.inherit(std::nullopt);
        theme.selectionInactiveBackground.inherit(std::nullopt);
        theme.white.inherit(std::nullopt);
        theme.yellow.inherit(std::nullopt);
    }
}

Nui::ElementRenderer TerminalOptions::render()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;

    return fragment(
        fontFamily(language->getObserved("settings", "terminalOptions", "fontFamily")),
        fontSize(language->getObserved("settings", "terminalOptions", "fontSize")),
        lineHeight(language->getObserved("settings", "terminalOptions", "lineHeight")),
        cursorBlink(language->getObserved("settings", "terminalOptions", "cursorBlink")),
        renderer(language->getObserved("settings", "terminalOptions", "renderer")),
        letterSpacing(language->getObserved("settings", "terminalOptions", "letterSpacing")),
        subgroup(
            {.engagedStatus = &themeEngaged,
                .groupTitle = language->getObserved("settings", "terminalOptions", "themeSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                theme.background(language->getObserved("settings", "terminalOptions", "theme", "background")),
                theme.black(language->getObserved("settings", "terminalOptions", "theme", "black")),
                theme.blue(language->getObserved("settings", "terminalOptions", "theme", "blue")),
                theme.brightBlack(language->getObserved("settings", "terminalOptions", "theme", "brightBlack")),
                theme.brightBlue(language->getObserved("settings", "terminalOptions", "theme", "brightBlue")),
                theme.brightCyan(language->getObserved("settings", "terminalOptions", "theme", "brightCyan")),
                theme.brightGreen(language->getObserved("settings", "terminalOptions", "theme", "brightGreen")),
                theme.brightMagenta(language->getObserved("settings", "terminalOptions", "theme", "brightMagenta")),
                theme.brightRed(language->getObserved("settings", "terminalOptions", "theme", "brightRed")),
                theme.brightWhite(language->getObserved("settings", "terminalOptions", "theme", "brightWhite")),
                theme.brightYellow(language->getObserved("settings", "terminalOptions", "theme", "brightYellow")),
                theme.cursor(language->getObserved("settings", "terminalOptions", "theme", "cursor")),
                theme.cursorAccent(language->getObserved("settings", "terminalOptions", "theme", "cursorAccent")),
                theme.cyan(language->getObserved("settings", "terminalOptions", "theme", "cyan")),
                theme.foreground(language->getObserved("settings", "terminalOptions", "theme", "foreground")),
                theme.green(language->getObserved("settings", "terminalOptions", "theme", "green")),
                theme.magenta(language->getObserved("settings", "terminalOptions", "theme", "magenta")),
                theme.red(language->getObserved("settings", "terminalOptions", "theme", "red")),
                theme.selectionBackground(
                    language->getObserved("settings", "terminalOptions", "theme", "selectionBackground")
                ),
                theme.selectionForeground(
                    language->getObserved("settings", "terminalOptions", "theme", "selectionForeground")
                ),
                theme.selectionInactiveBackground(
                    language->getObserved("settings", "terminalOptions", "theme", "selectionInactiveBackground")
                ),
                theme.white(language->getObserved("settings", "terminalOptions", "theme", "white")),
                theme.yellow(language->getObserved("settings", "terminalOptions", "theme", "yellow"))
            )
        )
    );
}
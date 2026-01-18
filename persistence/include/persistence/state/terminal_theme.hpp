#include <persistence/state_core.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Persistence
{
    struct TerminalTheme : public DefaultMissingMember
    {
        std::optional<std::string> background{std::nullopt};
        std::optional<std::string> black{std::nullopt};
        std::optional<std::string> blue{std::nullopt};
        std::optional<std::string> brightBlack{std::nullopt};
        std::optional<std::string> brightBlue{std::nullopt};
        std::optional<std::string> brightCyan{std::nullopt};
        std::optional<std::string> brightGreen{std::nullopt};
        std::optional<std::string> brightMagenta{std::nullopt};
        std::optional<std::string> brightRed{std::nullopt};
        std::optional<std::string> brightWhite{std::nullopt};
        std::optional<std::string> brightYellow{std::nullopt};
        std::optional<std::string> cursor{std::nullopt};
        std::optional<std::string> cursorAccent{std::nullopt};
        std::optional<std::string> cyan{std::nullopt};
        std::optional<std::vector<std::string>> extendedAnsi{std::nullopt};
        std::optional<std::string> foreground{std::nullopt};
        std::optional<std::string> green{std::nullopt};
        std::optional<std::string> magenta{std::nullopt};
        std::optional<std::string> red{std::nullopt};
        std::optional<std::string> selectionBackground{std::nullopt};
        std::optional<std::string> selectionForeground{std::nullopt};
        std::optional<std::string> selectionInactiveBackground{std::nullopt};
        std::optional<std::string> white{std::nullopt};
        std::optional<std::string> yellow{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        TerminalTheme,
        (),
        (background,
            black,
            blue,
            brightBlack,
            brightBlue,
            brightCyan,
            brightGreen,
            brightMagenta,
            brightRed,
            brightWhite,
            brightYellow,
            cursor,
            cursorAccent,
            cyan,
            extendedAnsi,
            foreground,
            green,
            magenta,
            red,
            selectionBackground,
            selectionForeground,
            selectionInactiveBackground,
            white,
            yellow)
    )
}
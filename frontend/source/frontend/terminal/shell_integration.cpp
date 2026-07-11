#include <frontend/terminal/shell_integration.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ShellIntegration
{
    namespace
    {
        // The hooks print the command verbatim. Guards keep bash from reporting what it runs behind
        // the user's back (prompt command, completion, the hook itself).
        constexpr std::string_view bashBootstrap =
            R"(__nui_preexec(){ [ -n "$COMP_LINE" ] && return; case "$BASH_COMMAND" in __nui_*) return;; esac; [ "$BASH_COMMAND" = "$PROMPT_COMMAND" ] && return; printf "\033]633;E;%s\007" "$BASH_COMMAND"; }; trap "__nui_preexec" DEBUG)";

        constexpr std::string_view zshBootstrap =
            R"(__nui_preexec(){ printf "\033]633;E;%s\007" "$1"; }; autoload -Uz add-zsh-hook; add-zsh-hook preexec __nui_preexec)";

        constexpr std::string_view fishBootstrap =
            R"(function __nui_preexec --on-event fish_preexec; printf "\033]633;E;%s\007" "$argv[1]"; end)";

        bool isHexDigit(char character)
        {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        }

        int hexValue(char character)
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return character - 'A' + 10;
        }

        /// Decodes the VS Code escapes; anything else stays as it is, because our own hooks print the
        /// command without escaping it.
        std::string unescape(std::string_view escaped)
        {
            std::string result;
            result.reserve(escaped.size());
            for (std::size_t index = 0; index < escaped.size(); ++index)
            {
                if (escaped[index] != '\\' || index + 1 >= escaped.size())
                {
                    result += escaped[index];
                    continue;
                }

                const auto next = escaped[index + 1];
                if (next == '\\')
                {
                    result += '\\';
                    ++index;
                }
                else if (next == 'x' && index + 3 < escaped.size() && isHexDigit(escaped[index + 2]) && isHexDigit(escaped[index + 3]))
                {
                    result += static_cast<char>(hexValue(escaped[index + 2]) * 16 + hexValue(escaped[index + 3]));
                    index += 3;
                }
                else
                {
                    result += escaped[index];
                }
            }
            return result;
        }

        std::string trim(std::string value)
        {
            const auto isSpace = [](char character) {
                return std::isspace(static_cast<unsigned char>(character)) != 0;
            };
            value.erase(value.begin(), std::ranges::find_if_not(value, isSpace));
            value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
            return value;
        }
    } // namespace

    ShellKind detectShellKind(std::filesystem::path const& command)
    {
        auto name = command.filename().string();
        if (name.ends_with(".exe"))
            name.resize(name.size() - 4);
        std::ranges::transform(name, name.begin(), [](char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        });

        if (name == "bash" || name == "sh")
            return ShellKind::Bash;
        if (name == "zsh")
            return ShellKind::Zsh;
        if (name == "fish")
            return ShellKind::Fish;
        return ShellKind::Unknown;
    }

    std::string bootstrap(ShellKind kind)
    {
        switch (kind)
        {
            case ShellKind::Bash:
                return std::string{bashBootstrap};
            case ShellKind::Zsh:
                return std::string{zshBootstrap};
            case ShellKind::Fish:
                return std::string{fishBootstrap};
            case ShellKind::Unknown:
                return {};
        }
        return {};
    }

    std::string remoteBootstrap()
    {
        // The guard is the part that makes this safe to fire blindly: fish parses the line (test,
        // [, && and eval all exist there and single quotes never expand), finds both version
        // variables empty and therefore never evaluates the posix body it could not parse. Shells
        // that are neither bash nor zsh do nothing for the same reason. The body itself must stay
        // free of single quotes, it lives inside them.
        return R"([ -n "$BASH_VERSION$ZSH_VERSION" ] && eval 'if [ -n "$BASH_VERSION" ]; then )" +
            std::string{bashBootstrap} + R"(; else )" + std::string{zshBootstrap} + R"(; fi')";
    }

    std::optional<std::string> commandFromOscPayload(std::string const& payload)
    {
        constexpr std::string_view executeSubcode = "E;";
        if (!payload.starts_with(executeSubcode))
            return std::nullopt;

        const auto command = trim(unescape(std::string_view{payload}.substr(executeSubcode.size())));
        if (command.empty())
            return std::nullopt;
        return command;
    }
}

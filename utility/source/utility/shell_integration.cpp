#include <utility/shell_integration.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ShellIntegration
{
    namespace
    {
        // The hooks print the command in the VS Code dialect: backslash, semicolon and the control
        // characters a terminal would drop or act on inside an OSC string become \\ and \xHH, so a
        // command reaches us byte for byte, newlines included.
        // bash reports the line from its own history, because BASH_COMMAND holds the alias expanded form and the DEBUG trap fires once per pipeline
        // stage; the remembered history number collapses those fires to one report per typed line
        // and keeps prompt command chains quiet. A line the shell itself drops from history
        // (ignorespace, ignoredups) is not reported, which errs on the quiet side.
        // __nui_init runs once at install: the bootstrap line itself is in history unless
        // ignorespace dropped it, so it is deleted there, and the history number current at install
        // is remembered. Otherwise the first fire (PROMPT_COMMAND) would report the bootstrap or
        // the command typed before it.
        // The DEBUG trap fires for every simple command, loop iterations included, so looking at
        // history (a fork) has to stay rare. From bash 5.1 on, HISTCMD in the trap is the history
        // number of the running line: the last reported number means a line already reported (loop
        // iterations, the next command of the line) or one the shell dropped, settled without a
        // fork. Older versions report 1 there, so PROMPT_COMMAND arms the hook for them instead and
        // only a report disarms it: prompt commands added after ours cost a fork but cannot swallow
        // the next line.
        // A DEBUG trap installed before ours keeps running ahead of it, with its exit status.
        constexpr std::string_view bashBootstrap =
            R"(__nui_preexec(){ [ -n "$COMP_LINE" ] && return; case "$BASH_COMMAND" in __nui_*) return;; esac; if [ -n "$__nui_histcmd" ]; then [ "$HISTCMD" = "$__nui_last_history" ] && return; else [ -z "$__nui_armed" ] && return; fi; local h n; h=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null); h="${h#"${h%%[![:space:]]*}"}"; [ -z "$h" ] && return; n="${h%%[[:space:]]*}"; n="${n%\*}"; [ "$n" = "$__nui_last_history" ] && return; __nui_last_history="$n"; __nui_armed=; h="${h#*[[:space:]]}"; local b="\\" l r t a e; printf -v l "\n"; printf -v r "\r"; printf -v t "\t"; printf -v a "\a"; printf -v e "\033"; h=${h//"$b"/"$b$b"}; h=${h//";"/"${b}x3b"}; h=${h//"$l"/"${b}x0a"}; h=${h//"$r"/"${b}x0d"}; h=${h//"$t"/"${b}x09"}; h=${h//"$a"/"${b}x07"}; h=${h//"$e"/"${b}x1b"}; printf "\033]633;E;%s\007" "$h"; }; )"
            R"(__nui_init(){ local h n l p; h=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null); h="${h#"${h%%[![:space:]]*}"}"; n="${h%%[[:space:]]*}"; case "$h" in *__nui_preexec*) builtin history -d "${n%\*}" 2>/dev/null; h=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null); h="${h#"${h%%[![:space:]]*}"}"; n="${h%%[[:space:]]*}";; esac; __nui_last_history="${n%\*}"; printf -v l "\n"; )"
            R"(if (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then __nui_histcmd=1; else PROMPT_COMMAND="${PROMPT_COMMAND:+$PROMPT_COMMAND$l}__nui_armed=1"; fi; )"
            R"(p=${__nui_trap#"trap -- "}; p=${p%" DEBUG"}; eval "p=$p"; case "$p" in *__nui_preexec*) __nui_trap=$p;; "") __nui_trap=__nui_preexec;; *) __nui_trap="$p${l}__nui_s=\$?; __nui_preexec; __nui_ret \"\$__nui_s\"";; esac; }; )"
            R"(__nui_ret(){ return "$1"; }; __nui_trap=$(trap -p DEBUG); __nui_init; unset -f __nui_init; trap -- "$__nui_trap" DEBUG; unset __nui_trap)";

        constexpr std::string_view zshBootstrap =
            R"(__nui_preexec(){ local c="$1" b="\\" l r t a e; printf -v l "\n"; printf -v r "\r"; printf -v t "\t"; printf -v a "\a"; printf -v e "\033"; c=${c//$b/$b$b}; c=${c//;/${b}x3b}; c=${c//$l/${b}x0a}; c=${c//$r/${b}x0d}; c=${c//$t/${b}x09}; c=${c//$a/${b}x07}; c=${c//$e/${b}x1b}; printf "\033]633;E;%s\007" "$c"; }; autoload -Uz add-zsh-hook; add-zsh-hook preexec __nui_preexec)";

        constexpr std::string_view fishBootstrap =
            R"(function __nui_preexec --on-event fish_preexec; set -l c (string replace -a -- \\ \\\\ $argv[1]); set c (string join -- \\x0a $c); set c (string replace -a -- \; \\x3b $c); set c (string replace -a -- \r \\x0d $c); set c (string replace -a -- \t \\x09 $c); set c (string replace -a -- \a \\x07 $c); set c (string replace -a -- \e \\x1b $c); printf "\033]633;E;%s\007" "$c"; end)";

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

        /// Decodes the VS Code escapes; a backslash that starts no valid escape stays as it is.
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
        std::ranges::transform(name, name.begin(), [](char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        });
        if (name.ends_with(".exe"))
            name.resize(name.size() - 4);

        if (name == "bash")
            return ShellKind::Bash;
        if (name == "sh")
            return ShellKind::Sh;
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
            case ShellKind::Sh:
                return remoteBootstrap();
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

    std::size_t echoCount(ShellKind kind)
    {
        return kind == ShellKind::Fish ? 3 : 2;
    }

    std::optional<std::string> commandFromOscPayload(std::string const& payload)
    {
        constexpr std::string_view executeSubcode = "E;";
        if (!payload.starts_with(executeSubcode))
            return std::nullopt;

        // E;<escaped command>[;<nonce>]: a literal semicolon is always escaped, so the first raw one
        // ends the command.
        auto escaped = std::string_view{payload}.substr(executeSubcode.size());
        escaped = escaped.substr(0, escaped.find(';'));
        const auto command = trim(unescape(escaped));
        // Our own bootstrap is never a user command, whatever path reports it.
        if (command.empty() || command.contains("__nui_preexec"))
            return std::nullopt;
        return command;
    }
}

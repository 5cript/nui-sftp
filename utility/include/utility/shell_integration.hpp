#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief Shell integration for the "smart" history capture mode.
 *
 * The shells announce the command they are about to run by emitting an OSC 633 sequence
 * (`ESC ] 633 ; E ; <command> BEL`, the dialect VS Code introduced) from a preexec hook. The hook is
 * installed by writing a one line bootstrap into the shell's stdin right after a fresh channel
 * opened. xterm's own parser picks the sequence up again on the way back, see TerminalChannel.
 */
namespace ShellIntegration
{
    /**
     * @brief The shells a preexec hook can be installed in.
     */
    enum class ShellKind
    {
        Unknown,
        Bash,
        /** @brief A posix sh that may be bash in disguise or dash, so it gets the guarded bootstrap. */
        Sh,
        Zsh,
        Fish,
    };

    /**
     * @brief Guesses the shell from the executable that is being run, e.g. "/usr/bin/zsh" -> Zsh.
     */
    ShellKind detectShellKind(std::filesystem::path const& command);

    /**
     * @brief The bootstrap line for a known shell, without a trailing newline.
     *
     * Empty for ShellKind::Unknown; use remoteBootstrap() when the shell cannot be known up front.
     */
    std::string bootstrap(ShellKind kind);

    /**
     * @brief The bootstrap line for a shell that is only known at runtime, e.g. behind ssh.
     *
     * Installs the bash or the zsh hook, whichever fits, and does nothing at all in any other shell
     * (fish included, which is why the guard is written so that fish can parse but never run it).
     */
    std::string remoteBootstrap();

    /**
     * @brief Environment variable a local shell receives its bootstrap in.
     *
     * The name carries the marker the bash bootstrap looks for to drop its own line from history.
     */
    constexpr std::string_view bootstrapVariable = "__nui_preexec_bootstrap";

    /**
     * @brief The short line that runs the bootstrap a local shell received in bootstrapVariable.
     *
     * A macOS tty drops typeahead past 1024 bytes while the shell is still starting, the bash
     * bootstrap is twice that. Empty for ShellKind::Unknown.
     */
    std::string environmentBootstrap(ShellKind kind);

    /**
     * @brief How often the shell echoes a bootstrap that was typed ahead: once from the tty, once
     *        from the line editor's redraw, and for fish once more when it repaints the accepted line.
     *
     * ShellKind::Unknown stands for the remote bootstrap, which only ever runs in bash or zsh.
     */
    std::size_t echoCount(ShellKind kind);

    /**
     * @brief Extracts the command from an OSC 633 payload, e.g. "E;git status" -> "git status".
     *
     * The payload is what xterm hands to the OSC handler, so without the leading "633;". Everything
     * but the E subcode is dropped (returns nullopt), as are empty commands. The VS Code escapes
     * (`\xHH` and `\\`) are decoded, so an integration script other than ours also works.
     */
    std::optional<std::string> commandFromOscPayload(std::string const& payload);

    /**
     * @brief The OSC code the preexec hooks and the handler agree on.
     */
    constexpr int oscCode = 633;
}

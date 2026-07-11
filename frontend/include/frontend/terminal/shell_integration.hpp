#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

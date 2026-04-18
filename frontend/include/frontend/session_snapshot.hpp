#pragma once

#include <frontend/resumable_op.hpp>

#include <ids/ids.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/terminal_options.hpp>
#include <persistence/state/termios.hpp>

#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/side/side_implementation.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Snapshot of a file-explorer side's user-visible state.
 */
struct FileExplorerSideSnapshot
{
    std::filesystem::path currentPath;
    NuiFileExplorer::Flavor flavor{NuiFileExplorer::Flavor::Icons};
    bool showHiddenFiles{false};
    unsigned int iconSize{0};
    unsigned int iconSpacing{0};
    std::pair<NuiFileExplorer::SortCriterion, bool> sort{NuiFileExplorer::SortCriterion::Name, true};
    std::optional<bool> placesOpen{std::nullopt};
    bool placesWide{false};
};

/**
 * @brief Local-shell process hand-off record.  The backend ProcessStore keeps
 *        the process alive across the SSH Session destruction, so the new
 *        Session's aux engine can adopt the process by id without re-spawning.
 */
struct LocalShellAdoption
{
    /// Doubles as the ChannelId on both sides.
    Ids::ChannelId processId;
    /// Matches the layout entry "local-shell:<name>".
    std::string shellConfigName;
    /// Last-known cmdline — used for the inner Lumino tab title.
    std::string cmdline;
    Persistence::TerminalOptions terminalOptions{};
    Persistence::Termios termios{};
    Persistence::ExecutingSessionOptions execOpts{};
    /// xterm serializeAddon dump, replayed after adoption.
    std::string savedScrollback;
    /**
     * @brief Backend-chosen RPC name for stdout chunks (e.g.
     *        "execTerminalStdout_<localId>").  The adopting engine registers
     *        fresh handlers at this exact name so the backend keeps routing
     *        output correctly without knowing the frontend swapped engines.
     */
    std::string stdoutReceptacle;
    /// Backend RPC name for stderr chunks; see stdoutReceptacle.
    std::string stderrReceptacle;
};

/**
 * @brief Full snapshot extracted from a dying Session, injected into the
 *        replacement Session via its constructor.  Consumed exactly once.
 */
struct SessionSnapshot
{
    /**
     * @brief Scrollback per primary (SSH) channel, in FSM::forEachChannel(PrimaryOnly)
     *        iteration order.  The replacement Session walks its newly-materialized
     *        primary channels in factory-call order (which is the restored Lumino
     *        layout's tab order) and replays slot-by-slot.
     */
    std::vector<std::string> primaryChannelScrollback;

    FileExplorerSideSnapshot local;
    FileExplorerSideSnapshot remote;

    std::string remoteUsername;
    bool sftpWasOpen{false};

    std::vector<ResumableOp> inFlightOps;
    std::vector<LocalShellAdoption> ejectedLocalShells;

    /**
     * @brief Saved Lumino layout from contentPanelManager.getPanelLayout — passed
     *        to initializeLayout on the new Session so exactly the same set of
     *        panels respawns in the same visual arrangement.
     */
    nlohmann::json luminoLayout;
};

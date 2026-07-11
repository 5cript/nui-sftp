#pragma once

#include <persistence/state_core.hpp>

#include <optional>

namespace Persistence
{
    /**
     * @brief How commands typed in a terminal are captured into the command history.
     */
    enum class HistoryCaptureMode
    {
        /// No capture at all.
        off,
        /// Line buffering of user keystrokes; works anywhere, but misreads TUIs, pagers and editors.
        simple,
        /// OSC 633 shell integration; accurate, but requires a cooperating shell.
        smart
    };
    BOOST_DESCRIBE_ENUM(HistoryCaptureMode, off, simple, smart);

    struct HistoryOptions : public DefaultMissingMember
    {
        std::optional<HistoryCaptureMode> captureMode{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(HistoryOptions, (), (captureMode))
}

#pragma once

#include <persistence/state_core.hpp>

#include <optional>
#include <filesystem>

namespace Persistence
{
    /**
     * @brief What happens when a file that can run code (a program, a script, a shortcut) is opened.
     */
    enum class RiskyFilePolicy
    {
        Allow,
        Warn,
        Never
    };
    BOOST_DESCRIBE_ENUM(RiskyFilePolicy, Allow, Warn, Never)

    struct LocalFilesystemOptions : public DefaultMissingMember
    {
        // by default, dont allow the user to delete files locally.
        bool preventDeletion{false};
        bool preventRename{false};
        bool preventCreateFile{false};
        bool preventCreateDirectory{false};
        std::optional<std::filesystem::path> homeOverride{std::nullopt};
        std::optional<std::filesystem::path> temporaryDownloadsDirectory{"%cache_home%/nui-sftp-downloads"};
        /// Optional so configurations written before it load without a warning; unset means Warn.
        std::optional<RiskyFilePolicy> openRiskyFiles{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(
        LocalFilesystemOptions,
        (),
        (preventDeletion,
            preventRename,
            preventCreateFile,
            preventCreateDirectory,
            homeOverride,
            temporaryDownloadsDirectory,
            openRiskyFiles)
    )
}
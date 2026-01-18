#pragma once

#include <persistence/state_core.hpp>

#include <optional>
#include <filesystem>

namespace Persistence
{
    struct LocalFilesystemOptions : public DefaultMissingMember
    {
        // by default, dont allow the user to delete files locally.
        bool preventDeletion{true};
        bool preventRename{false};
        bool preventCreateFile{false};
        bool preventCreateDirectory{false};
        std::optional<std::string> homeOverride{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(
        LocalFilesystemOptions,
        (),
        (preventDeletion, preventRename, preventCreateFile, preventCreateDirectory, homeOverride)
    )
}
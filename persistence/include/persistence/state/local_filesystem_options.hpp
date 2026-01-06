#pragma once

#include <persistence/state_core.hpp>

#include <optional>
#include <filesystem>

namespace Persistence
{
    struct LocalFilesystemOptions
    {
        // by default, dont allow the user to delete files locally.
        std::optional<bool> preventDeletion{true};
        std::optional<bool> preventRename{std::nullopt};
        std::optional<bool> preventCreateFile{std::nullopt};
        std::optional<bool> preventCreateDirectory{std::nullopt};
        std::optional<std::string> homeOverride{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(
        LocalFilesystemOptions,
        (),
        (preventDeletion, preventRename, preventCreateFile, preventCreateDirectory, homeOverride)
    )
}
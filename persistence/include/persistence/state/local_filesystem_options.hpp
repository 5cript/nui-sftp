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

        void useDefaultsFrom(LocalFilesystemOptions const& other);
    };
    void to_json(nlohmann::json& j, LocalFilesystemOptions const& options);
    void from_json(nlohmann::json const& j, LocalFilesystemOptions& options);
}
#include <persistence/state/local_filesystem_options.hpp>

namespace Persistence
{
    void LocalFilesystemOptions::useDefaultsFrom(LocalFilesystemOptions const& other)
    {
        if (!preventDeletion.has_value() && other.preventDeletion.has_value())
            preventDeletion = other.preventDeletion;
        if (!preventRename.has_value() && other.preventRename.has_value())
            preventRename = other.preventRename;
        if (!preventCreateFile.has_value() && other.preventCreateFile.has_value())
            preventCreateFile = other.preventCreateFile;
        if (!preventCreateDirectory.has_value() && other.preventCreateDirectory.has_value())
            preventCreateDirectory = other.preventCreateDirectory;
    }
    void to_json(nlohmann::json& j, LocalFilesystemOptions const& options)
    {
        j = nlohmann::json{};
        if (options.preventDeletion.has_value())
            j["preventDeletion"] = options.preventDeletion.value();
        if (options.preventRename.has_value())
            j["preventRename"] = options.preventRename.value();
        if (options.preventCreateFile.has_value())
            j["preventCreateFile"] = options.preventCreateFile.value();
        if (options.preventCreateDirectory.has_value())
            j["preventCreateDirectory"] = options.preventCreateDirectory.value();
    }
    void from_json(nlohmann::json const& j, LocalFilesystemOptions& options)
    {
        if (j.contains("preventDeletion"))
            options.preventDeletion = j.at("preventDeletion").get<bool>();
        if (j.contains("preventRename"))
            options.preventRename = j.at("preventRename").get<bool>();
        if (j.contains("preventCreateFile"))
            options.preventCreateFile = j.at("preventCreateFile").get<bool>();
        if (j.contains("preventCreateDirectory"))
            options.preventCreateDirectory = j.at("preventCreateDirectory").get<bool>();
    }
}
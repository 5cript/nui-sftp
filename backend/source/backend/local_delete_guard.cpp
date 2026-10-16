#include <backend/local_delete_guard.hpp>

#include <nui/backend/filesystem/special_paths.hpp>

std::optional<Paths::LocalPath> localHomeDirectory()
{
    auto home = Paths::LocalPath::make(Nui::resolvePath("%userprofile%"));
    if (!home)
        return std::nullopt;
    return *std::move(home);
}

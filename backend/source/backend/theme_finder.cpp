#include <backend/theme_finder.hpp>
#include <backend/resources.hpp>

#include <filesystem>

std::vector<std::filesystem::path> ThemeFinder::findAvailableThemes(std::filesystem::path const& relativeRoot)
{
    const auto dirs = getThemeDirs(relativeRoot);
    std::vector<std::filesystem::path> themes{};
    for (const auto& dir : dirs)
    {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
            continue;

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".css")
                themes.push_back(entry.path().stem());
        }
    }
    return themes;
}
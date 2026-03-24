#include <backend/theme_finder.hpp>
#include <backend/resources.hpp>
#include <log/log.hpp>

#include <filesystem>

ThemeFinder::ThemeFinder(std::filesystem::path const& relativeRoot, AppWideEvents& events)
    : themeReloadListener_{Nui::smartListen(
          events.onReloadThemes,
          [&events, relativeRoot](bool)
          {
              events.availableThemes = findAvailableThemes(relativeRoot);
              events.availableThemes.eventContext().sync();
          }
      )}
{}

std::vector<std::filesystem::path> ThemeFinder::findAvailableThemes(std::filesystem::path const& relativeRoot)
{
    const auto dirs = getThemeDirs(relativeRoot);
    std::vector<std::filesystem::path> themes{};
    Log::info("Finding available themes in {} candidate directories.", dirs.size());
    for (const auto& dir : dirs)
    {
        Log::info("Checking theme directory: {}", dir.generic_string());
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
        {
            Log::info("Skipping non-existing or non-directory theme path: {}", dir.generic_string());
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".css")
            {
                Log::info("Found theme file: {}", entry.path().generic_string());
                themes.push_back(entry.path().stem());
            }
        }
    }
    Log::info("Theme discovery complete, found {} themes.", themes.size());
    return themes;
}
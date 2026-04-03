#include <backend/theme_finder.hpp>
#include <utility/resources.hpp>
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
    const auto files = findFilesInSearchPaths(relativeRoot, "themes/*.css");
    std::vector<std::filesystem::path> themes;
    themes.reserve(files.size());
    for (const auto& file : files)
    {
        Log::info("Found theme file: {}", file.generic_string());
        themes.push_back(file.stem());
    }
    Log::info("Theme discovery complete, found {} themes.", themes.size());
    return themes;
}
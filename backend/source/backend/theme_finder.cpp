#include <backend/theme_finder.hpp>
#include <utility/resources.hpp>
#include <log/log.hpp>

#include <filesystem>

ThemeFinder::ThemeFinder(std::filesystem::path const& relativeRoot, AppWideEvents& events)
    : themeReloadListener_{Nui::smartListen(
          events.onReloadThemes,
          [&events, relativeRoot](bool)
          {
              Log::info("Reloading themes... relativeRoot='{}'", relativeRoot.generic_string());
              events.availableThemes = findAvailableThemes(relativeRoot);
              Log::info("availableThemes updated, count={}", events.availableThemes.value().size());
              events.availableThemes.eventContext().sync();
              Log::info("availableThemes sync done.");
          }
      )}
{
    Log::info("ThemeFinder constructed with relativeRoot='{}'", relativeRoot.generic_string());
}

std::vector<std::filesystem::path> ThemeFinder::findAvailableThemes(std::filesystem::path const& relativeRoot)
{
    Log::info("findAvailableThemes called with relativeRoot='{}'", relativeRoot.generic_string());
    std::error_code ec;
    const bool rootExists = std::filesystem::exists(relativeRoot, ec);
    Log::info("relativeRoot exists={}, ec='{}'", rootExists, ec.message());

    const auto files = findFilesInSearchPaths(relativeRoot, "themes/*.css");
    Log::info("findFilesInSearchPaths returned {} file(s).", files.size());
    std::vector<std::filesystem::path> themes;
    themes.reserve(files.size());
    for (const auto& file : files)
    {
        Log::info("Found theme file: '{}', stem='{}'", file.generic_string(), file.stem().generic_string());
        themes.push_back(file.stem());
    }
    Log::info("Theme discovery complete, found {} themes.", themes.size());
    return themes;
}
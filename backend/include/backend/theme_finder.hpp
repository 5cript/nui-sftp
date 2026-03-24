#pragma once

#include <events/app_wide_events.hpp>

#include <vector>
#include <string>

class ThemeFinder
{
  public:
    ThemeFinder(std::filesystem::path const& relativeRoot, AppWideEvents& events);

  private:
    static std::vector<std::filesystem::path> findAvailableThemes(std::filesystem::path const& relativeRoot);
    Nui::ListenRemover<decltype(AppWideEvents::onReloadThemes)> themeReloadListener_;
};
#pragma once

#include <unordered_map>
#include <string>

class Environment
{
  public:
    Environment() = default;
    Environment(
        bool clean,
        std::unordered_map<std::string, std::string> const& mergeIn,
        std::string const& pathMergeIn = {});

    std::unordered_map<std::string, std::string>& environment();

    /**
     *  @brief Loads the environment of this process, without the changes the AppImage launcher made.
     */
    void loadFromCurrent();
    void extendPath(std::string const& path, bool front = true);
    void merge(std::unordered_map<std::string, std::string> const& other, bool overwrite = true);

  private:
    /**
     *  @brief Puts back the values the AppImage launcher recorded before changing them.
     *
     *  The launcher stores each changed variable as NUI_SFTP_APPIMAGE_ORIGINAL_<name> and each variable
     *  it added as NUI_SFTP_APPIMAGE_UNSET_<name>. Child processes are host programs and must not inherit
     *  the bundled library paths.
     */
    void restoreHostEnvironment();

    std::unordered_map<std::string, std::string> environment_;
};

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace OpenerDetail
{
    /**
     * @brief Types Windows runs, installs or follows on top of what AssocIsDangerous reports.
     */
    inline constexpr std::array<std::string_view, 12> windowsRunnableExtensions{
        ".jar",
        ".py",
        ".pyw",
        ".pyz",
        ".application",
        ".appref-ms",
        ".appx",
        ".msix",
        ".appxbundle",
        ".msixbundle",
        ".appinstaller",
        ".settingcontent-ms",
    };

    /**
     * @brief Types LaunchServices runs, installs or follows instead of showing them.
     */
    inline constexpr std::array<std::string_view, 15> macRunnableExtensions{
        ".command",
        ".tool",
        ".terminal",
        ".pkg",
        ".mpkg",
        ".workflow",
        ".scpt",
        ".applescript",
        ".jar",
        ".webloc",
        ".inetloc",
        ".fileloc",
        ".prefpane",
        ".saver",
        ".mobileconfig",
    };

    /**
     * @brief Types a Linux desktop starts as programs or installs instead of showing them.
     */
    inline constexpr std::array<std::string_view, 9> linuxRunnableExtensions{
        ".desktop",
        ".appimage",
        ".run",
        ".jar",
        ".deb",
        ".rpm",
        ".flatpakref",
        ".flatpakrepo",
        ".snap",
    };

    /**
     * @brief True when the extension of @p path, compared ignoring ASCII case, is one of @p extensions (each written
     *        in lower case with its dot).
     */
    bool hasExtensionIn(std::filesystem::path const& path, std::span<std::string_view const> extensions);

    /**
     * @brief True when owner, group or others may execute.
     */
    bool hasExecuteBit(std::filesystem::perms permissions);

    /**
     * @brief True when @p path starts like a program: the ELF magic or a "#!" interpreter line.
     */
    bool hasExecutableContent(std::filesystem::path const& path);

    /**
     * @brief The Linux classification of @p path, see Opener::riskOf. A file with an execute bit counts as a program
     *        only when its content is one, since mounts without permissions (FAT, NTFS, SMB) mark every file
     *        executable. A symlink is judged by its own name and by its target's.
     */
    std::optional<std::string> linuxRiskOf(std::filesystem::path const& path);
}

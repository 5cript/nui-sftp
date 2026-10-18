#pragma once

#include <shared_data/opener_capabilities.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class Opener
{
  public:
    /**
     * @brief Connects to the platform's facilities for opening files.
     * @param portalParentWindow Parent window of xdg-desktop-portal dialogs on Linux ("wayland:<handle>",
     *        "x11:<id>" or empty), ignored on the other platforms.
     */
    explicit Opener(std::string portalParentWindow = {});
    ~Opener();
    Opener(Opener const&) = delete;
    Opener& operator=(Opener const&) = delete;
    Opener(Opener&&) noexcept;
    Opener& operator=(Opener&&) noexcept;

    std::expected<void, std::string> openFile(std::filesystem::path const& path, bool openWith);

    /**
     * @brief Why opening @p path could run code: a program, a script, a shortcut or an application bundle. Nothing
     *        for an ordinary document or a folder.
     * @return A localized message key describing the risk.
     */
    std::optional<std::string> riskOf(std::filesystem::path const& path) const;

    /**
     *  @brief Open the native file manager showing @p path.
     *  @details For a directory, opens the file manager at that directory. For a file, opens the
     *           parent directory and asks the file manager to highlight the file.
     */
    std::expected<void, std::string> openInFileManager(std::filesystem::path const& path);

    /**
     *  @brief Probe the platform's external-open facilities and report which calls are expected
     *         to succeed. Called once at startup by the frontend via RPC so it can gray out
     *         "Open" / "Open With" / "Open in File Manager" menu entries and warn the user.
     */
    SharedData::OpenerCapabilities capabilities() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
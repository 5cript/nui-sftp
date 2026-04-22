#pragma once

#include <shared_data/opener_capabilities.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

class Opener
{
  public:
    explicit Opener(void* nativeWindow = nullptr);
    ~Opener();
    Opener(Opener const&) = delete;
    Opener& operator=(Opener const&) = delete;
    Opener(Opener&&) noexcept;
    Opener& operator=(Opener&&) noexcept;

    std::expected<void, std::string> openFile(std::filesystem::path const& path, bool openWith);

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
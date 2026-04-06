#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

class Opener
{
  public:
    Opener();
    ~Opener();
    Opener(Opener const&) = delete;
    Opener& operator=(Opener const&) = delete;
    Opener(Opener&&) noexcept;
    Opener& operator=(Opener&&) noexcept;

    std::expected<void, std::string> openFile(std::filesystem::path const& path, bool openWith);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
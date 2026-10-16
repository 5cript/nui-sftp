#pragma once

#include <utility/temporary_directory.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace Utility
{
    /**
     * @brief The temporary directory of one test process, below @p base, so tests running in parallel share no files.
     *        A gtest death test runs in a re-executed copy of the test binary that dies on purpose: it reuses its
     *        parent's directory, handed over through the environment, and only the parent removes it.
     */
    class ProcessTemporaryDirectory
    {
      public:
        explicit ProcessTemporaryDirectory(std::filesystem::path const& base)
        {
            if (char const* inherited = std::getenv(environmentName); inherited != nullptr && *inherited != '\0')
            {
                path_ = std::filesystem::path{inherited};
                std::filesystem::create_directories(path_);
                return;
            }
            owned_.emplace(base, true);
            path_ = owned_->path();
#ifdef _WIN32
            ::_putenv_s(environmentName, path_.string().c_str());
#else
            ::setenv(environmentName, path_.c_str(), 1);
#endif
        }

        std::filesystem::path const& path() const noexcept
        {
            return path_;
        }

      private:
        static constexpr char const* environmentName = "NUI_SFTP_TEST_PROCESS_TEMPORARY";

        std::optional<TemporaryDirectory> owned_{};
        std::filesystem::path path_{};
    };
}

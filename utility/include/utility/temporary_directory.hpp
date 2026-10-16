#pragma once

#include <utility/link_safe_remove.hpp>
#include <paths/native.hpp>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fmt/format.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace Utility
{
    class TemporaryDirectory
    {
      public:
        TemporaryDirectory(std::filesystem::path path, bool randomSubFolder = true)
            : path_{std::move(path)}
        {
            using namespace std::string_literals;

            std::error_code error;
            bool valid = true;
            if (!std::filesystem::exists(path_, error))
            {
                valid = std::filesystem::create_directories(path_, error) && !error;
            }
            else if (!std::filesystem::is_directory(path_))
            {
                throw std::runtime_error(
                    fmt::format("Given path exists and is not a directory: {}", Paths::nativeToUtf8(path_))
                );
            }
            if (valid && randomSubFolder)
            {
                auto dir = path_ / ("temp_"s + boost::uuids::to_string(gen_()));
                std::filesystem::create_directory(dir);
                valid = std::filesystem::is_directory(dir);
                path_ = dir;
            }
            if (!valid)
            {
                throw std::runtime_error(
                    fmt::format("Could not setup temporary directory: {}", Paths::nativeToUtf8(path_))
                );
            }
        }
        ~TemporaryDirectory()
        {
            std::error_code error;

            // Never through a link or Windows junction a test left inside.
            if (!path_.empty())
                removeAllNoFollow(path_, error);
        }

        TemporaryDirectory(TemporaryDirectory const&) = delete;

        TemporaryDirectory(TemporaryDirectory&& t)
            : path_{std::move(t.path_)}
        {}

        TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;

        TemporaryDirectory& operator=(TemporaryDirectory&& t)
        {
            path_ = std::exchange(t.path_, {});
            return *this;
        }

        std::filesystem::path path() const noexcept
        {
            return path_;
        }

        operator std::filesystem::path() const noexcept
        {
            return path_;
        }

      private:
        std::filesystem::path path_;
        boost::uuids::random_generator gen_{};
    };
}
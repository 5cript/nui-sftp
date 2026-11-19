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
#include <system_error>

namespace Utility
{
    class TemporaryDirectory
    {
      public:
        /**
         * @brief Makes @p path a directory, and a directory of a random name in it when @p randomSubFolder is set,
         *        which the destructor removes.
         *
         * @throws std::runtime_error When that is not possible. What the constructor made until then is removed
         *         again, no destructor runs behind it.
         */
        TemporaryDirectory(std::filesystem::path path, bool randomSubFolder = true)
            : path_{std::move(path)}
        {
            using namespace std::string_literals;

            std::error_code error{};
            const auto existed = std::filesystem::exists(path_, error);
            if (existed && !std::filesystem::is_directory(path_, error))
            {
                throw std::runtime_error(
                    fmt::format("Given path exists and is not a directory: {}", Paths::nativeToUtf8(path_))
                );
            }
            const auto made = existed ? std::filesystem::path{} : makeDirectories(path_, error);
            if (error)
                failSetup(made, path_);
            if (randomSubFolder)
            {
                const auto directory = path_ / ("temp_"s + boost::uuids::to_string(gen_()));
                const auto madeDirectory = std::filesystem::create_directory(directory, error);
                if (error || !std::filesystem::is_directory(directory, error))
                {
                    auto toRemove = made;
                    if (toRemove.empty() && madeDirectory)
                        toRemove = directory;
                    failSetup(toRemove, directory);
                }
                path_ = directory;
            }
        }
        ~TemporaryDirectory()
        {
            std::error_code error;

            // Never follows a link or Windows junction inside.
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
        /**
         * @brief Makes @p path and the directories on the way to it that are missing.
         *
         * @return The topmost directory this call made, which holds every other one it made. Empty when it made
         *         none. Set also when @p error tells that a later one could not be made.
         */
        static std::filesystem::path makeDirectories(std::filesystem::path const& path, std::error_code& error)
        {
            std::filesystem::path made{};
            std::filesystem::path current{};
            for (auto part = path.begin(); part != path.end() && !error; ++part)
            {
                current /= *part;
                std::error_code ignored{};
                if (std::filesystem::is_directory(current, ignored))
                    continue;
                // True only for a directory this call made, so nothing that was there before is ever removed.
                if (std::filesystem::create_directory(current, error) && made.empty())
                    made = current;
            }
            return made;
        }

        /**
         * @brief Removes @p made, unless it is empty, and throws that @p directory could not be set up.
         */
        [[noreturn]] static void failSetup(std::filesystem::path const& made, std::filesystem::path const& directory)
        {
            std::error_code ignored{};
            // Never follows a link or Windows junction inside.
            if (!made.empty())
                removeAllNoFollow(made, ignored);
            throw std::runtime_error(
                fmt::format("Could not setup temporary directory: {}", Paths::nativeToUtf8(directory))
            );
        }

        std::filesystem::path path_;
        boost::uuids::random_generator gen_{};
    };
}
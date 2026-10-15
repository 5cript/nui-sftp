#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>

#include <fmt/format.h>

#include <fstream>
#include <system_error>

#ifdef _WIN32
#    include <windows.h>
#endif

namespace FileTracking
{
    namespace
    {
        std::filesystem::path bookkeepingDirectory(std::filesystem::path const& instanceDirectory)
        {
            return instanceDirectory / std::filesystem::path{bookkeepingDirectoryName};
        }

        bool fileExists(std::filesystem::path const& path)
        {
            std::error_code statusError;
            return std::filesystem::is_regular_file(path, statusError);
        }

        /**
         * @brief An instance directory of a build without the bookkeeping directory: only those have their lock and
         *        metadata next to the content, elsewhere files of those names are tracked content.
         */
        bool isLegacyInstance(std::filesystem::path const& instanceDirectory)
        {
            return !fileExists(bookkeepingPaths(instanceDirectory).lock) &&
                fileExists(legacyBookkeepingPaths(instanceDirectory).lock);
        }

        std::expected<nlohmann::json, std::string> readJsonFile(std::filesystem::path const& path)
        {
            std::ifstream input{path};
            if (!input)
                return std::unexpected(fmt::format("cannot open '{}'", Paths::nativeToUtf8(path)));
            auto parsed = nlohmann::json::parse(input, nullptr, false);
            if (parsed.is_discarded())
                return std::unexpected(fmt::format("'{}' is not valid JSON", Paths::nativeToUtf8(path)));
            return parsed;
        }

        std::expected<void, std::string> writeJsonFile(std::filesystem::path const& path, nlohmann::json const& json)
        {
            std::ofstream output{path, std::ios::trunc};
            output << json.dump(4);
            output.close();
            if (!output)
                return std::unexpected(fmt::format("cannot write '{}'", Paths::nativeToUtf8(path)));
            return {};
        }
    }

    BookkeepingPaths bookkeepingPaths(std::filesystem::path const& instanceDirectory)
    {
        const auto directory = bookkeepingDirectory(instanceDirectory);
        return {.lock = directory / "instance.lock", .metadata = directory / "metadata.json"};
    }

    BookkeepingPaths legacyBookkeepingPaths(std::filesystem::path const& instanceDirectory)
    {
        const auto instanceId = Paths::nativeToUtf8(instanceDirectory.filename());
        return {
            .lock = instanceDirectory / Paths::nativeFromUtf8(fmt::format("{}.lock", instanceId)),
            .metadata = instanceDirectory / "metadata.json",
        };
    }

    std::expected<void, std::string> createBookkeepingDirectory(std::filesystem::path const& instanceDirectory)
    {
        const auto directory = bookkeepingDirectory(instanceDirectory);
        std::error_code createError;
        std::filesystem::create_directories(directory, createError);
        if (createError)
            return std::unexpected(fmt::format(
                "cannot create '{}': {}", Paths::nativeToUtf8(directory), Utility::errorMessage(createError)
            ));
#ifdef _WIN32
        if (const auto attributes = GetFileAttributesW(directory.c_str()); attributes != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW(directory.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
#endif
        return {};
    }

    bool isBookkeepingPath(std::filesystem::path const& instanceDirectory, std::filesystem::path const& path)
    {
        const auto relative = path.lexically_relative(instanceDirectory);
        return !relative.empty() && *relative.begin() == std::filesystem::path{bookkeepingDirectoryName};
    }

    std::optional<std::filesystem::path> existingLockFile(std::filesystem::path const& instanceDirectory)
    {
        if (auto lock = bookkeepingPaths(instanceDirectory).lock; fileExists(lock))
            return lock;
        if (auto lock = legacyBookkeepingPaths(instanceDirectory).lock; fileExists(lock))
            return lock;
        return std::nullopt;
    }

    std::expected<std::optional<nlohmann::json>, std::string>
    readMetadata(std::filesystem::path const& instanceDirectory)
    {
        const auto metadata = [&instanceDirectory]() -> std::optional<std::filesystem::path>
        {
            if (auto current = bookkeepingPaths(instanceDirectory).metadata; fileExists(current))
                return current;
            if (auto legacy = legacyBookkeepingPaths(instanceDirectory).metadata;
                fileExists(legacy) && isLegacyInstance(instanceDirectory))
                return legacy;
            return std::nullopt;
        }();
        if (!metadata)
            return std::optional<nlohmann::json>{};
        return readJsonFile(*metadata).transform(
            [](nlohmann::json json)
            {
                return std::optional<nlohmann::json>{std::move(json)};
            }
        );
    }

    std::expected<void, std::string>
    writeMetadata(std::filesystem::path const& instanceDirectory, nlohmann::json const& metadata)
    {
        const bool mirrorLegacy = isLegacyInstance(instanceDirectory) &&
            fileExists(legacyBookkeepingPaths(instanceDirectory).metadata);
        return createBookkeepingDirectory(instanceDirectory)
            .and_then(
                [&]()
                {
                    return writeJsonFile(bookkeepingPaths(instanceDirectory).metadata, metadata);
                }
            )
            .and_then(
                [&]() -> std::expected<void, std::string>
                {
                    if (!mirrorLegacy)
                        return {};
                    return writeJsonFile(legacyBookkeepingPaths(instanceDirectory).metadata, metadata);
                }
            );
    }
}

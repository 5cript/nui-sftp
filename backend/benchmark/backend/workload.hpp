#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Benchmark
{
    /**
     * @brief A set of files of one size moved in one transfer: a single file, or directories holding many.
     */
    struct Workload
    {
        std::string name{};
        std::uint64_t fileCount{1};
        std::uint64_t fileSize{0};
        /** @brief Files per directory below the root when there is more than one. */
        std::uint64_t filesPerDirectory{100};

        bool isSingleFile() const noexcept
        {
            return fileCount == 1;
        }

        std::uint64_t totalBytes() const noexcept
        {
            return fileCount * fileSize;
        }
    };

    enum class Direction
    {
        Download,
        Upload,
    };

    std::string_view toString(Direction direction);

    /**
     * @brief One transfer of a workload. For a single file both paths name the file, otherwise the root directory.
     */
    struct Transfer
    {
        Direction direction{Direction::Download};
        Workload const* workload{nullptr};
        /** @brief The source of an upload or the target of a download. */
        std::filesystem::path localPath{};
        /** @brief The source of a download or the target of an upload. */
        std::string remotePath{};
        /** @brief The files below the source root, relative and sorted. Empty for a single file. */
        std::vector<std::string> files{};
    };

    /**
     * @brief Creates the files of @p workload below @p root with random content, in the layout the server uses.
     *
     * @return The created files relative to @p root, empty for a single file which is @p root itself.
     */
    std::vector<std::string> createLocalSource(Workload const& workload, std::filesystem::path const& root);
}

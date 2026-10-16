#pragma once

#include <algorithm>
#include <optional>
#include <span>

namespace Paths
{
    /**
     * @brief The closest directory that holds every one of @p paths below it. A path without a parent (a root)
     *        stands for itself, nothing for no paths. Works for LocalPath and RemotePath.
     */
    template <typename PathT>
    std::optional<PathT> commonParentOf(std::span<PathT const> paths)
    {
        if (paths.empty())
            return std::nullopt;
        auto candidate = paths.front().parent();
        if (!candidate)
            return paths.front();
        const auto containsAll = [&paths](PathT const& directory)
        {
            return std::ranges::all_of(
                paths,
                [&directory](PathT const& path)
                {
                    return path != directory && path.isWithin(directory);
                }
            );
        };
        while (candidate && !containsAll(*candidate))
            candidate = candidate->parent();
        return candidate;
    }
}

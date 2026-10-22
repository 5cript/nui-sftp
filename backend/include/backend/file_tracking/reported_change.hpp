#pragma once

#include <backend/file_tracking/file_action.hpp>

#include <filesystem>
#include <functional>

namespace FileTracking
{
    /**
     * @brief A change of a watched path as its watch reports it, once noise is dropped and content changes settled,
     *        see ChangeReporter.
     */
    struct ReportedChange
    {
        FileAction action;
        /** @brief Spelled like the watched directory, canonical when that is. */
        std::filesystem::path path;
        /** @brief Where a move came from, empty for every other change. */
        std::filesystem::path oldPath{};
        /** @brief Whether path was a directory when the change was seen. */
        bool isDirectory{false};

        friend bool operator==(ReportedChange const&, ReportedChange const&) = default;
    };

    /**
     * @brief Receives the changes of a watch, called on the strand of the watch.
     */
    using ChangeSink = std::function<void(ReportedChange const& change)>;
}

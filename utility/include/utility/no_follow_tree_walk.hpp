#pragma once

#include <utility/link_safe_remove.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Utility
{
    /**
     * @brief Walks a path and, for a real directory, the tree below it in bounded steps without following links or
     *        junctions. It either counts the entries or removes them, the content of a directory before the
     *        directory itself. A link is counted or removed itself.
     *
     * An item that cannot be removed does not stop the walk, only the directories containing it stay.
     */
    class NoFollowTreeWalk
    {
      public:
        enum class Mode
        {
            Count,
            Remove
        };

        NoFollowTreeWalk(std::filesystem::path root, Mode mode);
        ~NoFollowTreeWalk() = default;
        NoFollowTreeWalk(NoFollowTreeWalk const&) = delete;
        NoFollowTreeWalk& operator=(NoFollowTreeWalk const&) = delete;
        NoFollowTreeWalk(NoFollowTreeWalk&&) = default;
        NoFollowTreeWalk& operator=(NoFollowTreeWalk&&) = default;

        /**
         * @brief Works until the walk is done or @p deadline has passed, handling at least one entry per call.
         * @return True when the walk is done.
         */
        bool step(std::chrono::steady_clock::time_point deadline);

        bool done() const noexcept
        {
            return done_;
        }

        /**
         * @brief The entries counted or removed so far, the root included.
         */
        std::uintmax_t entries() const noexcept
        {
            return entries_;
        }

        /**
         * @brief Count mode: the size of the regular files counted so far.
         */
        std::uintmax_t bytes() const noexcept
        {
            return bytes_;
        }

        /**
         * @brief Remove mode: the items that could not be removed or listed, by their own path.
         */
        std::vector<RemoveFailure> const& failures() const noexcept
        {
            return failures_;
        }

        /**
         * @brief The entry handled last, empty before the first step.
         */
        std::filesystem::path const& lastPath() const noexcept
        {
            return lastPath_;
        }

      private:
        struct Frame
        {
            std::filesystem::path directory;
            std::filesystem::directory_iterator iterator;
            /** @brief Something below could not be removed, so the directory stays. */
            bool keepsContent{false};
        };

        void start();
        void handleEntry(std::filesystem::directory_entry const& entry);
        void enter(std::filesystem::path const& directory);
        void finishDirectory();
        void finishItem(std::filesystem::path const& path, NoFollowKind kind, bool directoryEntry, std::uintmax_t size);
        void fail(std::filesystem::path const& path, std::error_code const& error);

      private:
        std::filesystem::path root_;
        Mode mode_;
        bool started_{false};
        bool done_{false};
        std::vector<Frame> stack_{};
        std::uintmax_t entries_{0};
        std::uintmax_t bytes_{0};
        std::vector<RemoveFailure> failures_{};
        std::filesystem::path lastPath_{};
    };
}

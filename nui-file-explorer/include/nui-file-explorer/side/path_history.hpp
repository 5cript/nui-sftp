#pragma once

#include <filesystem>
#include <optional>
#include <deque>

namespace NuiFileExplorer
{
    class PathHistory
    {
      public:
        PathHistory() = default;

        /// Record a new navigation.  Call this whenever the user explicitly
        /// navigates somewhere (path bar Enter, suggestion click, directory
        /// double-click, etc.).  Do NOT call when moving through history itself.
        void push(std::filesystem::path path)
        {
            // No-op if we're already sitting on this path.
            if (!history_.empty() && history_[cursor_] == path)
                return;

            // Drop everything after the cursor (forward stack).
            if (cursor_ + 1 < history_.size())
                history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_) + 1, history_.end());

            history_.push_back(std::move(path));
            cursor_ = history_.size() - 1;

            // Evict the oldest entry if we exceed the cap.  cursor_ is always at
            // the end after a push, so decrementing it by one corrects for the
            // removed front element.
            if (history_.size() > maxSize)
            {
                history_.pop_front();
                --cursor_;
            }
        }

        /// Move one step back.  Returns the path to navigate to, or nullopt if
        /// already at the beginning of the history.
        std::optional<std::filesystem::path> back()
        {
            if (!canGoBack())
                return std::nullopt;
            --cursor_;
            return history_[cursor_];
        }

        /// Move one step forward.  Returns the path to navigate to, or nullopt if
        /// already at the most recent entry.
        std::optional<std::filesystem::path> forward()
        {
            if (!canGoForward())
                return std::nullopt;
            ++cursor_;
            return history_[cursor_];
        }

        bool canGoBack() const
        {
            return !history_.empty() && cursor_ > 0;
        }
        bool canGoForward() const
        {
            return !history_.empty() && cursor_ + 1 < history_.size();
        }

        /// The path currently shown (i.e. the entry at the cursor).
        std::optional<std::filesystem::path> current() const
        {
            if (history_.empty())
                return std::nullopt;
            return history_[cursor_];
        }

      private:
        static constexpr std::size_t maxSize = 100;

        std::deque<std::filesystem::path> history_{};
        std::size_t cursor_{0};
    };
}
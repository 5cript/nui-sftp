#pragma once

#include <paths/path_segment.hpp>
#include <paths/relative_path.hpp>

#include <string>
#include <string_view>
#include <vector>
#include <type_traits>
#include <filesystem>
#include <expected>
#include <functional>

namespace Utility
{
    class BaseDirectoryWalker
    {
      public:
        virtual ~BaseDirectoryWalker() = default;

        std::uint64_t totalBytes() const
        {
            return totalBytes_;
        }
        std::size_t currentIndex() const
        {
            return currentIndex_;
        }
        virtual std::size_t totalEntries() const = 0;

        virtual bool completed() const = 0;

      protected:
        std::size_t currentIndex_{0};
        std::uint64_t totalBytes_{0};
    };

    /**
     * @brief Walks a directory tree breadth first. The walker never sees the root path: the scanner lists the
     *        directory at a path relative to the root, and entries are named relative to their parent. The root
     *        is entry 0 and has an empty name.
     */
    template <typename EntryT, typename WalkErrorType, typename ScannerT, bool ScannerIncludesDotAndDotDot>
    requires std::
        is_invocable_r_v<std::expected<std::vector<EntryT>, WalkErrorType>, ScannerT, Paths::RelativePath const&>
        class DeepDirectoryWalker : public BaseDirectoryWalker
    {
      public:
        template <typename ForwardingScannerT = ScannerT>
        requires std::is_same_v<std::decay_t<ForwardingScannerT>, ScannerT>
        explicit DeepDirectoryWalker(ForwardingScannerT&& scanner)
            : scanner_{std::forward<ForwardingScannerT>(scanner)}
            , entries_{EntryT{
                  .type = EntryT::FileType::Directory,
              }}
        {}

        std::vector<EntryT> ejectEntries() &&
        {
            return std::move(entries_);
        }

        std::vector<EntryT> const& entries() const
        {
            return entries_;
        }

        std::size_t totalEntries() const override
        {
            return entries_.size();
        }

        void reset()
        {
            entries_ = {EntryT{
                .type = EntryT::FileType::Directory,
            }};
            rejectedEntries_.clear();
            currentIndex_ = 0;
            totalBytes_ = 0;
        }

        /**
         * @brief Walk a single iteration.
         *
         * @return std::expected<bool, WalkErrorType> Returns false if there are more entries to process, true if done.
         * On error returns unexpected with the error.
         */
        std::expected<bool, WalkErrorType> walk()
        {
            if (completed())
                return false;

            if (entries_[currentIndex_].isDirectory())
            {
                auto result = scanner_(relativePath(entries_[currentIndex_]));
                if (!result)
                    return std::unexpected(std::move(result).error());

                moveEntries(std::move(result).value(), currentIndex_);
                ++currentIndex_;
            }
            else
            {
                // Log::warn(
                //     "DeepDirectoryWalker: Encountered non-directory entry '{}' during walk at a point where a "
                //     "directory was expected, skipping.",
                //     relativePath(entries_[currentIndex_]).value()
                // );
            }

            // Move over all non dirs
            for (; currentIndex_ < entries_.size(); ++currentIndex_)
            {
                auto const& current = entries_[currentIndex_];
                if (current.isRegularFile())
                    totalBytes_ += current.size;
                if (current.isDirectory())
                    break;
            }

            return completed();
        }

        /**
         * @brief The path of a walked entry below the root, empty for the root.
         */
        Paths::RelativePath relativePath(EntryT const& entry) const
        {
            if (!entry.parent)
                return {};
            const auto parentIndex = entry.parent.value();
            if (parentIndex >= entries_.size())
                throw std::out_of_range("Parent index is out of range");
            // Only valid segments are adopted, see moveEntries.
            return entry.nameSegment().transform(
                [this, parentIndex](Paths::PathSegment const& segment)
                {
                    return directoryPaths_[parentIndex] / segment;
                }
            );
        }

        /**
         * @brief The path of a rejected entry below the root for display, its own name as listed.
         */
        std::string describeRejected(EntryT const& entry) const
        {
            if (!entry.parent || entry.parent.value() >= entries_.size())
                return nameOf(entry);
            const auto parent = relativePath(entries_[entry.parent.value()]);
            return parent.empty() ? nameOf(entry) : parent.value() + "/" + nameOf(entry);
        }

        std::expected<void, WalkErrorType> walkAll()
        {
            decltype(walk()) res;
            do
            {
                res = walk();
                if (!res)
                    return std::unexpected(std::move(res).error());
            } while (!res.value());
            return {};
        }

        bool completed() const override
        {
            return currentIndex_ >= entries_.size();
        }

        /**
         * @brief Entries whose name is not a valid path segment, like ".." or "a/b" from a server or a name that was
         *        not UTF-8. They are never walked or returned by @ref entries, their parent index is set so their
         *        full path can be shown.
         */
        std::vector<EntryT> const& rejectedEntries() const
        {
            return rejectedEntries_;
        }

      private:
        /**
         * @brief Adopts one listing. Every "." and ".." is dropped and every other name has to be a valid path
         *        segment: a name like "x/.." would turn into a path outside the walked tree.
         */
        void moveEntries(std::vector<EntryT>&& newEntries, std::size_t parent)
        {
            for (auto& entry : newEntries)
            {
                entry.parent = parent;
                const auto name = nameOf(entry);
                if (name == "." || name == "..")
                    continue;
                auto segment = entry.nameSegment();
                if (!segment.has_value())
                {
                    rejectedEntries_.push_back(std::move(entry));
                    continue;
                }
                entries_.push_back(std::move(entry));
            }
        }

        static std::string const& nameOf(EntryT const& entry)
        {
            return entry.name;
        }

      private:
        ScannerT scanner_;
        std::vector<EntryT> entries_{};
        std::vector<EntryT> rejectedEntries_{};
    };
}
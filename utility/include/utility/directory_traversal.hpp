#pragma once

#include <paths/path_error.hpp>
#include <paths/path_segment.hpp>
#include <paths/relative_path.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <string>
#include <string_view>
#include <vector>
#include <type_traits>
#include <filesystem>
#include <expected>
#include <functional>
#include <ranges>
#include <string_view>

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
     * @brief The path of @p entry below the root of its walk, following the parent indices through @p entries.
     *        Empty for the root, which is the entry without a parent. Refused when a name on the way is no path
     *        segment or a parent index lies outside @p entries.
     */
    template <typename EntryT>
    std::expected<Paths::RelativePath, Paths::PathError>
    walkedRelativePath(std::vector<EntryT> const& entries, EntryT const& entry)
    {
        std::vector<EntryT const*> ancestry;
        for (auto const* current = &entry; current->parent; current = &entries[*current->parent])
        {
            if (*current->parent >= entries.size())
                return std::unexpected(
                    Paths::PathError{.reason = Paths::PathRejection::UnknownParent, .input = current->name}
                );
            ancestry.push_back(current);
        }
        Paths::RelativePath relative{};
        for (auto const* ancestor : ancestry | std::views::reverse)
        {
            auto segment = ancestor->nameSegment();
            if (!segment)
                return std::unexpected(std::move(segment).error());
            relative = relative / *segment;
        }
        return relative;
    }

    /**
     * @brief @p entry of a walk for display: @p root followed by the names as listed on the way down to it, so it can
     *        name an entry whose name is no path segment. Starts below a parent index outside @p entries.
     */
    template <typename EntryT>
    std::string describeWalkedEntry(std::string_view root, std::vector<EntryT> const& entries, EntryT const& entry)
    {
        std::vector<std::string_view> names;
        for (auto const* current = &entry; current->parent; current = &entries[*current->parent])
        {
            names.push_back(current->name);
            if (*current->parent >= entries.size())
                break;
        }
        const auto below = fmt::format("{}", fmt::join(names | std::views::reverse, "/"));
        if (root.empty() || below.empty())
            return root.empty() ? below : std::string{root};
        return root.ends_with('/') ? fmt::format("{}{}", root, below) : fmt::format("{}/{}", root, below);
    }

    /**
     * @brief Walks a directory tree breadth first. The walker never sees the root path: the scanner lists the
     *        directory at a path relative to the root, and entries are named relative to their parent. The root
     *        is entry 0 and has an empty name. Every "." and ".." a scanner lists is dropped.
     */
    template <typename EntryT, typename WalkErrorType, typename ScannerT>
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
            , directoryPaths_(1)
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
            directoryPaths_.assign(1, Paths::RelativePath{});
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
                return true;

            if (entries_[currentIndex_].isDirectory())
            {
                auto result = scanner_(directoryPaths_[currentIndex_]);
                if (!result)
                    return std::unexpected(std::move(result).error());

                moveEntries(std::move(result).value(), currentIndex_);
                ++currentIndex_;
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
         * @brief The path of a walked entry below the root, empty for the root. Refused for an entry whose parent is
         *        not among the walked entries.
         */
        std::expected<Paths::RelativePath, Paths::PathError> relativePath(EntryT const& entry) const
        {
            if (!entry.parent)
                return Paths::RelativePath{};
            const auto parentIndex = entry.parent.value();
            if (parentIndex >= entries_.size())
                return std::unexpected(
                    Paths::PathError{.reason = Paths::PathRejection::UnknownParent, .input = entry.name}
                );
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
            return describeWalkedEntry({}, entries_, entry);
        }

        std::expected<void, WalkErrorType> walkAll()
        {
            while (true)
            {
                auto result = walk();
                if (!result)
                    return std::unexpected(std::move(result).error());
                if (*result)
                    return {};
            }
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
                auto const& name = entry.name;
                if (name == "." || name == "..")
                    continue;
                auto segment = entry.nameSegment();
                if (!segment.has_value())
                {
                    rejectedEntries_.push_back(std::move(entry));
                    continue;
                }
                directoryPaths_.push_back(
                    entry.isDirectory() ? directoryPaths_[parent] / *segment : Paths::RelativePath{}
                );
                entries_.push_back(std::move(entry));
            }
        }

      private:
        ScannerT scanner_;
        std::vector<EntryT> entries_{};
        /** @brief Index aligned with entries_: the path of each directory below the root, empty for the others. */
        std::vector<Paths::RelativePath> directoryPaths_{};
        std::vector<EntryT> rejectedEntries_{};
    };
}

#pragma once

#include <log/log.hpp>

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

    template <typename EntryT, typename WalkErrorType, typename ScannerT, bool ScannerIncludesDotAndDotDot>
    requires std::
        is_invocable_r_v<std::expected<std::vector<EntryT>, WalkErrorType>, ScannerT, std::filesystem::path const&>
        class DeepDirectoryWalker : public BaseDirectoryWalker
    {
      public:
        template <typename ForwardingScannerT = ScannerT>
        requires std::is_same_v<std::decay_t<ForwardingScannerT>, ScannerT>
        DeepDirectoryWalker(std::filesystem::path rootPath, ForwardingScannerT&& scanner)
            : rootPath_{std::move(rootPath)}
            , scanner_{std::forward<ForwardingScannerT>(scanner)} // root is part of the entries:
            , entries_{EntryT{
                  .path = rootPath_,
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
                .path = rootPath_,
                .type = EntryT::FileType::Directory,
            }};
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
                auto result = scanner_(fullPath(entries_[currentIndex_]));
                if (!result)
                    return std::unexpected(std::move(result).error());

                moveEntries(std::move(result).value(), currentIndex_);
                ++currentIndex_;
            }
            else
            {
                Log::warn(
                    "DeepDirectoryWalker: Encountered non-directory entry '{}' during walk at a point where a "
                    "directory was expected, skipping.",
                    fullPath(entries_[currentIndex_]).generic_string()
                );
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

        std::filesystem::path fullPath(EntryT const& entry) const
        {
            if (entry.parent)
            {
                const auto parentIndex = entry.parent.value();
                if (parentIndex >= entries_.size())
                    throw std::out_of_range("Parent index is out of range");
                return fullPath(entries_[parentIndex]) / entry.path;
            }
            else
                return entry.path;
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

      private:
        void moveEntries(std::vector<EntryT>&& newEntries, std::size_t parent)
        {
            if constexpr (ScannerIncludesDotAndDotDot)
            {
                auto iter = std::make_move_iterator(begin(newEntries));
                auto endMarker = std::make_move_iterator(end(newEntries));
                bool foundOneWasDot = false;
                for (; iter != endMarker; ++iter)
                {
                    auto val{*iter};
                    if (val.path == ".")
                    {
                        foundOneWasDot = true;
                        ++iter;
                        break;
                    }
                    else if (val.path == "..")
                    {
                        ++iter;
                        break;
                    }
                    val.parent = parent;
                    entries_.push_back(std::move(val));
                }

                if (foundOneWasDot)
                {
                    for (; iter != endMarker; ++iter)
                    {
                        auto val{*iter};
                        if (val.path == "..")
                        {
                            ++iter;
                            break;
                        }
                        val.parent = parent;
                        entries_.push_back(std::move(val));
                    }
                }
                else
                {
                    for (; iter != endMarker; ++iter)
                    {
                        auto val{*iter};
                        if (val.path == ".")
                        {
                            ++iter;
                            break;
                        }
                        val.parent = parent;
                        entries_.push_back(std::move(val));
                    }
                }

                std::transform(
                    iter,
                    endMarker,
                    std::back_inserter(entries_),
                    [parent](EntryT&& entry)
                    {
                        entry.parent = parent;
                        return std::move(entry);
                    }
                );
            }
            else
            {
                std::transform(
                    std::make_move_iterator(begin(newEntries)),
                    std::make_move_iterator(end(newEntries)),
                    std::back_inserter(entries_),
                    [parent](EntryT&& entry)
                    {
                        entry.parent = parent;
                        return std::move(entry);
                    }
                );
            }
        }

      private:
        std::filesystem::path rootPath_;
        ScannerT scanner_;
        std::vector<EntryT> entries_{};
    };
}
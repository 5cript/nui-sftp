#pragma once

#include <shared_data/directory_entry.hpp>
#include <shared_data/sync/scan_node.hpp>
#include <utility/directory_traversal.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Directory walker that produces a sorted @ref ScanNode tree directly.
     *
     * Mirrors the scanner contract of @ref Utility::DeepDirectoryWalker — the caller
     * supplies a scanner that returns a batch of @ref DirectoryEntry for one directory —
     * but internally stitches results into a nested ScanNode tree with:
     *
     *  - sibling sort by name applied per directory as entries arrive, and
     *  - @ref ScanNode::subtreeFileCount / @ref ScanNode::subtreeByteTotal accumulated
     *    when the subtree completes.
     *
     * No second pass is needed. Pointer stability relies on reserving each node's children
     * vector before pushing — a node's children vector is only appended to in a single
     * scanner invocation, so a `reserve(N) + N pushes` sequence never reallocates.
     */
    template <typename WalkErrorType, typename ScannerT, bool ScannerIncludesDotAndDotDot>
    requires std::is_invocable_r_v<
        std::expected<std::vector<SharedData::DirectoryEntry>, WalkErrorType>,
        ScannerT,
        std::filesystem::path const&>
    class TreeDirectoryWalker : public Utility::BaseDirectoryWalker
    {
      public:
        template <typename ForwardingScannerT = ScannerT>
        requires std::is_same_v<std::decay_t<ForwardingScannerT>, ScannerT>
        TreeDirectoryWalker(std::filesystem::path rootPath, ForwardingScannerT&& scanner)
            : rootPath_{std::move(rootPath)}
            , scanner_{std::forward<ForwardingScannerT>(scanner)}
        {
            root_.type = SharedData::FileType::Directory;
            root_.childrenKnown = true;
            queue_.push_back(QueuedDir{.node = &root_, .fullPath = rootPath_, .depth = 0});
            ++totalEntries_;
        }

        /**
         * @brief Moves the built tree out of the walker. Must only be called after
         *        @ref completed() returns true.
         */
        ScanNode ejectTree() &&
        {
            finalizeCounts(root_);
            return std::move(root_);
        }

        std::size_t totalEntries() const override
        {
            return totalEntries_;
        }

        bool completed() const override
        {
            return queue_.empty();
        }

        void reset()
        {
            root_ = ScanNode{};
            root_.type = SharedData::FileType::Directory;
            root_.childrenKnown = true;
            queue_.clear();
            queue_.push_back(QueuedDir{.node = &root_, .fullPath = rootPath_, .depth = 0});
            currentIndex_ = 0;
            totalBytes_ = 0;
            totalEntries_ = 1;
        }

        /**
         * @brief Scan one directory from the BFS queue. @return true when the walker is done.
         */
        std::expected<bool, WalkErrorType> walk()
        {
            if (queue_.empty())
                return true;

            const auto pending = queue_.front();
            queue_.pop_front();

            auto result = scanner_(pending.fullPath);
            if (!result)
                return std::unexpected(std::move(result).error());

            auto rawChildren = std::move(result).value();

            if constexpr (ScannerIncludesDotAndDotDot)
            {
                std::erase_if(rawChildren, [](SharedData::DirectoryEntry const& entry) {
                    return entry.path == "." || entry.path == "..";
                });
            }

            std::sort(
                rawChildren.begin(),
                rawChildren.end(),
                [](SharedData::DirectoryEntry const& lhs, SharedData::DirectoryEntry const& rhs) {
                    return lhs.path.filename().generic_string() < rhs.path.filename().generic_string();
                }
            );

            // Reserve up front so `&children.back()` stays valid through every push_back
            // inside this loop — lets us enqueue directory pointers in the same pass.
            pending.node->children.reserve(rawChildren.size());
            for (auto& raw : rawChildren)
            {
                if (raw.type == SharedData::FileType::Regular)
                    totalBytes_ += raw.size;

                pending.node->children.push_back(ScanNode{
                    .name = raw.path.filename().generic_string(),
                    .type = raw.type,
                    .size = raw.size,
                    .mtime = raw.mtime,
                    .mtimeNsec = raw.mtimeNsec,
                    .permissions = raw.permissions,
                    .linkTarget = std::move(raw.linkTarget),
                    .uid = raw.uid,
                    .gid = raw.gid,
                    .children = {},
                    .subtreeFileCount = 0,
                    .subtreeByteTotal = 0,
                    .childrenKnown = true,
                });
                ++totalEntries_;

                if (raw.type == SharedData::FileType::Directory)
                {
                    auto& inserted = pending.node->children.back();
                    queue_.push_back(QueuedDir{
                        .node = &inserted,
                        .fullPath = pending.fullPath / inserted.name,
                        .depth = pending.depth + 1,
                    });
                }
            }

            ++currentIndex_;
            return queue_.empty();
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

      private:
        /**
         * @brief Post-order subtree-metric fill. Runs once, on eject.
         */
        static void finalizeCounts(ScanNode& node)
        {
            node.subtreeFileCount = 0;
            node.subtreeByteTotal = 0;
            for (auto& child : node.children)
            {
                if (child.type == SharedData::FileType::Directory)
                {
                    finalizeCounts(child);
                    node.subtreeFileCount += child.subtreeFileCount;
                    node.subtreeByteTotal += child.subtreeByteTotal;
                }
                else
                {
                    node.subtreeFileCount += 1;
                    node.subtreeByteTotal += child.size;
                }
            }
        }

        struct QueuedDir
        {
            ScanNode* node;
            std::filesystem::path fullPath;
            std::size_t depth;
        };

        std::filesystem::path rootPath_;
        ScannerT scanner_;
        ScanNode root_{};
        std::deque<QueuedDir> queue_{};
        std::size_t totalEntries_{0};
    };
}

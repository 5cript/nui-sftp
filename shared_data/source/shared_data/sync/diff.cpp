#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_tree_node.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SharedData::Sync
{
    namespace
    {
        constexpr std::uint64_t progressCheckpointInterval = 512;

        bool isHidden(std::string const& name)
        {
            return !name.empty() && name.front() == '.';
        }

        std::string joinRel(std::string const& parentRelKey, std::string const& name)
        {
            if (parentRelKey.empty())
                return name;
            std::string out;
            out.reserve(parentRelKey.size() + 1 + name.size());
            out.append(parentRelKey);
            out.push_back('/');
            out.append(name);
            return out;
        }

        /**
         * @brief Resolve how a one-sided local entry should be emitted.
         *        Returns std::nullopt when the entry should be skipped.
         */
        std::optional<Action> chooseLocalOnlyAction(ScanNode const& node, DiffOptions const& options)
        {
            if (options.direction == Direction::Download)
            {
                if (!options.actionDelete)
                    return std::nullopt;
                // Non-recursive mode: if we never descended into this directory we cannot
                // safely delete it — the user hasn't seen its contents.
                if (node.type == FileType::Directory && !node.childrenKnown)
                    return std::nullopt;
                return Action::DeleteLocal;
            }
            if (!options.actionUpload)
                return std::nullopt;
            return Action::Upload;
        }

        /**
         * @brief Mirror of @ref chooseLocalOnlyAction for a one-sided remote entry.
         */
        std::optional<Action> chooseRemoteOnlyAction(ScanNode const& node, DiffOptions const& options)
        {
            if (options.direction == Direction::Upload)
            {
                if (!options.actionDelete)
                    return std::nullopt;
                if (node.type == FileType::Directory && !node.childrenKnown)
                    return std::nullopt;
                return Action::DeleteRemote;
            }
            if (!options.actionDownload)
                return std::nullopt;
            return Action::Download;
        }

        DiffTreeNode makeLocalOnlyRow(ScanNode const& node, std::string const& relKey, Action action)
        {
            const bool isDir = node.type == FileType::Directory;
            return DiffTreeNode{
                .relKey = relKey,
                .name = node.name,
                .action = action,
                .isDirectory = isDir,
                .hasLocalSide = true,
                .hasRemoteSide = false,
                .localSize = node.size,
                .remoteSize = 0,
                .localMtime = node.mtime,
                .remoteMtime = 0,
                .directChildCount = isDir ? static_cast<std::uint32_t>(node.children.size()) : 0u,
                .descendantItemCount = isDir ? node.subtreeFileCount : 1ull,
                .descendantByteTotal = isDir ? node.subtreeByteTotal : node.size,
            };
        }

        DiffTreeNode makeRemoteOnlyRow(ScanNode const& node, std::string const& relKey, Action action)
        {
            const bool isDir = node.type == FileType::Directory;
            return DiffTreeNode{
                .relKey = relKey,
                .name = node.name,
                .action = action,
                .isDirectory = isDir,
                .hasLocalSide = false,
                .hasRemoteSide = true,
                .localSize = 0,
                .remoteSize = node.size,
                .localMtime = 0,
                .remoteMtime = node.mtime,
                .directChildCount = isDir ? static_cast<std::uint32_t>(node.children.size()) : 0u,
                .descendantItemCount = isDir ? node.subtreeFileCount : 1ull,
                .descendantByteTotal = isDir ? node.subtreeByteTotal : node.size,
            };
        }

        DiffTreeNode makeFileDifferRow(
            ScanNode const& localNode,
            ScanNode const& remoteNode,
            std::string const& relKey,
            Action action
        )
        {
            return DiffTreeNode{
                .relKey = relKey,
                .name = localNode.name,
                .action = action,
                .isDirectory = false,
                .hasLocalSide = true,
                .hasRemoteSide = true,
                .localSize = localNode.size,
                .remoteSize = remoteNode.size,
                .localMtime = localNode.mtime,
                .remoteMtime = remoteNode.mtime,
                .directChildCount = 0,
                .descendantItemCount = 1,
                .descendantByteTotal = action == Action::Upload ? localNode.size : remoteNode.size,
            };
        }

        void emitOneSided(
            DiffTreeNode row,
            Action action,
            std::string const& parentRelKey,
            DiffSink& sink
        )
        {
            switch (action)
            {
                case Action::Upload:
                    sink.emitUpload(std::move(row), parentRelKey);
                    return;
                case Action::Download:
                    sink.emitDownload(std::move(row), parentRelKey);
                    return;
                case Action::DeleteLocal:
                case Action::DeleteRemote:
                    sink.emitDelete(std::move(row), parentRelKey);
                    return;
            }
        }

        /**
         * @brief Forward declaration — recursion point for directory pairs.
         */
        void mergeChildren(
            std::vector<ScanNode> const& localChildren,
            std::vector<ScanNode> const& remoteChildren,
            std::string const& parentRelKey,
            DiffOptions const& options,
            DiffSink& sink,
            std::uint64_t& comparedCounter
        );

        void handleBothPresent(
            ScanNode const& localChild,
            ScanNode const& remoteChild,
            std::string const& relKey,
            std::string const& parentRelKey,
            DiffOptions const& options,
            DiffSink& sink,
            std::uint64_t& comparedCounter
        )
        {
            const bool sameDir =
                localChild.type == FileType::Directory && remoteChild.type == FileType::Directory;
            if (sameDir)
            {
                if (options.recursive)
                    mergeChildren(localChild.children, remoteChild.children, relKey, options, sink, comparedCounter);
                return;
            }

            if (!entriesDiffer(localChild, remoteChild))
                return;

            // Direction routing: Upload-only → Upload; Download-only → Download;
            // Both → mtime-newer wins (local >= remote favors Upload).
            if (options.direction == Direction::Upload)
            {
                if (options.actionUpload)
                    sink.emitUpload(makeFileDifferRow(localChild, remoteChild, relKey, Action::Upload), parentRelKey);
                return;
            }
            if (options.direction == Direction::Download)
            {
                if (options.actionDownload)
                    sink.emitDownload(
                        makeFileDifferRow(localChild, remoteChild, relKey, Action::Download), parentRelKey
                    );
                return;
            }
            // Both direction.
            const bool localNewer = localChild.mtime >= remoteChild.mtime;
            if (localNewer)
            {
                if (options.actionUpload)
                    sink.emitUpload(makeFileDifferRow(localChild, remoteChild, relKey, Action::Upload), parentRelKey);
            }
            else
            {
                if (options.actionDownload)
                    sink.emitDownload(
                        makeFileDifferRow(localChild, remoteChild, relKey, Action::Download), parentRelKey
                    );
            }
        }

        void mergeChildren(
            std::vector<ScanNode> const& localChildren,
            std::vector<ScanNode> const& remoteChildren,
            std::string const& parentRelKey,
            DiffOptions const& options,
            DiffSink& sink,
            std::uint64_t& comparedCounter
        )
        {
            std::size_t li = 0;
            std::size_t ri = 0;

            const auto bumpAndCheckpoint = [&]() {
                ++comparedCounter;
                if (comparedCounter % progressCheckpointInterval == 0)
                    sink.onProgress(comparedCounter);
            };

            while (li < localChildren.size() || ri < remoteChildren.size())
            {
                if (sink.cancelled())
                    return;

                const bool hasL = li < localChildren.size();
                const bool hasR = ri < remoteChildren.size();

                // Pick the smaller name (or the only side available).
                const bool takeBoth = hasL && hasR && localChildren[li].name == remoteChildren[ri].name;
                const bool takeLeft = hasL && (!hasR || localChildren[li].name < remoteChildren[ri].name);
                const bool takeRight = hasR && (!hasL || remoteChildren[ri].name < localChildren[li].name);

                if (takeBoth)
                {
                    auto const& lc = localChildren[li];
                    auto const& rc = remoteChildren[ri];
                    if (!options.ignoreHidden || !isHidden(lc.name))
                    {
                        const auto relKey = joinRel(parentRelKey, lc.name);
                        handleBothPresent(lc, rc, relKey, parentRelKey, options, sink, comparedCounter);
                    }
                    ++li;
                    ++ri;
                    bumpAndCheckpoint();
                }
                else if (takeLeft)
                {
                    auto const& lc = localChildren[li];
                    if (!options.ignoreHidden || !isHidden(lc.name))
                    {
                        if (auto action = chooseLocalOnlyAction(lc, options); action)
                        {
                            const auto relKey = joinRel(parentRelKey, lc.name);
                            emitOneSided(makeLocalOnlyRow(lc, relKey, *action), *action, parentRelKey, sink);
                        }
                    }
                    ++li;
                    bumpAndCheckpoint();
                }
                else if (takeRight)
                {
                    auto const& rc = remoteChildren[ri];
                    if (!options.ignoreHidden || !isHidden(rc.name))
                    {
                        if (auto action = chooseRemoteOnlyAction(rc, options); action)
                        {
                            const auto relKey = joinRel(parentRelKey, rc.name);
                            emitOneSided(makeRemoteOnlyRow(rc, relKey, *action), *action, parentRelKey, sink);
                        }
                    }
                    ++ri;
                    bumpAndCheckpoint();
                }
                else
                {
                    // Both exhausted — loop will exit.
                    break;
                }
            }
        }
    }

    bool entriesDiffer(ScanNode const& localNode, ScanNode const& remoteNode)
    {
        if (localNode.type != remoteNode.type)
            return true;
        if (localNode.type == FileType::Symlink)
        {
            if (localNode.linkTarget && remoteNode.linkTarget)
                return *localNode.linkTarget != *remoteNode.linkTarget;
            return false;
        }
        if (localNode.size != remoteNode.size)
            return true;
        if (localNode.mtime != remoteNode.mtime)
            return true;
        return false;
    }

    void diffScanTrees(
        ScanNode const& local,
        ScanNode const& remote,
        DiffOptions const& options,
        DiffSink& sink
    )
    {
        std::uint64_t compared = 0;
        mergeChildren(local.children, remote.children, std::string{}, options, sink, compared);
        // Final heartbeat so observers see the completed count even if it didn't
        // land on a checkpoint boundary.
        sink.onProgress(compared);
    }
}

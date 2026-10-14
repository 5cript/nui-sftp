#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_tree_node.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SharedData::Sync
{
    namespace
    {
        constexpr std::uint64_t progressCheckpointInterval = 512;

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
                // Never delete a directory whose content the user cannot see in the dialog.
                if (node.type == FileType::Directory && (!node.childrenKnown || !options.recursive))
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
                if (node.type == FileType::Directory && (!node.childrenKnown || !options.recursive))
                    return std::nullopt;
                return Action::DeleteRemote;
            }
            if (!options.actionDownload)
                return std::nullopt;
            return Action::Download;
        }

        /**
         * @brief Row for an entry present on both sides. The transferred (source) side decides the
         *        shape, so a local directory replacing a remote file is a directory row.
         */
        DiffTreeNode
        makeTwoSidedRow(ScanNode const& localNode, ScanNode const& remoteNode, std::string const& relKey, Action action)
        {
            auto const& source = action == Action::Upload ? localNode : remoteNode;
            const bool isDirectory = source.type == FileType::Directory;
            return DiffTreeNode{
                .relKey = relKey,
                .name = localNode.name,
                .action = action,
                .isDirectory = isDirectory,
                .hasLocalSide = true,
                .hasRemoteSide = true,
                .localSize = localNode.size,
                .remoteSize = remoteNode.size,
                .localMtime = localNode.mtime,
                .remoteMtime = remoteNode.mtime,
                .directChildCount = isDirectory ? static_cast<std::uint32_t>(source.children.size()) : 0u,
                .descendantItemCount = isDirectory ? source.subtreeFileCount : 1ull,
                .descendantByteTotal = isDirectory ? source.subtreeByteTotal : source.size,
                .typeConflict = localNode.type != remoteNode.type,
            };
        }

        /**
         * @brief Resolves the action for an entry that differs on both sides. Direction Both lets the
         *        newer side win, ties go to the upload.
         */
        std::optional<Action>
        chooseTwoSidedAction(ScanNode const& localNode, ScanNode const& remoteNode, DiffOptions const& options)
        {
            const bool upload = options.direction == Direction::Upload ||
                (options.direction == Direction::Both && localNode.mtime >= remoteNode.mtime);
            if (upload)
                return options.actionUpload ? std::optional{Action::Upload} : std::nullopt;
            return options.actionDownload ? std::optional{Action::Download} : std::nullopt;
        }

        void emitOneSided(
            DiffTreeNode row,
            Action action,
            std::string const& parentRelKey,
            DiffOptions const& options,
            DiffSink& sink
        )
        {
            // Without recursion a one-sided directory is created, not filled.
            if (row.isDirectory && !options.recursive)
            {
                row.directChildCount = 0;
                row.descendantItemCount = 0;
                row.descendantByteTotal = 0;
            }
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

        std::optional<BlockReason> blockReasonOf(ScanNode const& node)
        {
            switch (node.availability)
            {
                case Availability::Listed:
                    return std::nullopt;
                case Availability::Unreadable:
                    return BlockReason::Unreadable;
                case Availability::Excluded:
                    return BlockReason::Excluded;
                case Availability::InvalidName:
                    return BlockReason::InvalidName;
            }
            return BlockReason::Excluded;
        }

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
            // Unknown or untransferable on one side: neither side may be changed for this name.
            if (auto reason = blockReasonOf(localChild).or_else([&]() { return blockReasonOf(remoteChild); }); reason)
            {
                sink.onBlocked(relKey, *reason);
                return;
            }

            // Transferring a directory onto a symlink writes through the link, out of the sync root.
            const auto isPair = [&](FileType first, FileType second)
            {
                return (localChild.type == first && remoteChild.type == second) ||
                    (localChild.type == second && remoteChild.type == first);
            };
            if (isPair(FileType::Directory, FileType::Symlink))
            {
                sink.onBlocked(relKey, BlockReason::TypeConflict);
                return;
            }

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

            const auto action = chooseTwoSidedAction(localChild, remoteChild, options);
            if (!action)
                return;
            auto row = makeTwoSidedRow(localChild, remoteChild, relKey, *action);
            if (*action == Action::Upload)
                sink.emitUpload(std::move(row), parentRelKey);
            else
                sink.emitDownload(std::move(row), parentRelKey);
        }

        std::string foldedCase(std::string_view name)
        {
            std::string folded{name};
            for (auto& character : folded)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            return folded;
        }

        /**
         * @brief Names of one directory that would meet as one file on a case folding filesystem: a local-only and a
         *        remote-only name that differ only in case, and remote names that differ only in case when the local
         *        filesystem folds case. Transferring either could overwrite the other and a delete could hit both.
         *        ASCII letters only.
         */
        std::unordered_set<std::string> caseConflictsOf(
            std::vector<ScanNode> const& localChildren,
            std::vector<ScanNode> const& remoteChildren,
            DiffOptions const& options
        )
        {
            const auto hasUpper = [](std::vector<ScanNode> const& children)
            {
                return std::ranges::any_of(
                    children,
                    [](ScanNode const& node)
                    {
                        return std::ranges::any_of(node.name, [](char character) { return character >= 'A' && character <= 'Z'; });
                    }
                );
            };
            // Without an upper case letter folding changes nothing, which is the common case.
            if (!hasUpper(localChildren) && !hasUpper(remoteChildren))
                return {};

            struct Variants
            {
                std::vector<std::string_view> local;
                std::vector<std::string_view> remote;
            };
            std::unordered_map<std::string, Variants> byFolded;
            for (auto const& node : localChildren)
                byFolded[foldedCase(node.name)].local.push_back(node.name);
            for (auto const& node : remoteChildren)
                byFolded[foldedCase(node.name)].remote.push_back(node.name);

            std::unordered_set<std::string> conflicts;
            for (auto const& [folded, variants] : byFolded)
            {
                const bool crossSide = std::ranges::any_of(
                    variants.local,
                    [&variants](std::string_view local)
                    {
                        return std::ranges::any_of(
                            variants.remote, [local](std::string_view remote) { return remote != local; }
                        );
                    }
                );
                const bool remoteVariants = options.localCaseInsensitive && variants.remote.size() > 1;
                if (!crossSide && !remoteVariants)
                    continue;
                for (auto const name : variants.local)
                    conflicts.emplace(name);
                for (auto const name : variants.remote)
                    conflicts.emplace(name);
            }
            return conflicts;
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
            const auto caseConflicts = caseConflictsOf(localChildren, remoteChildren, options);
            const auto blockedForCase = [&](std::string const& name)
            {
                if (!caseConflicts.contains(name))
                    return false;
                sink.onBlocked(joinRel(parentRelKey, name), BlockReason::CaseConflict);
                return true;
            };

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
                    if ((!options.ignoreHidden || !isHiddenName(lc.name)) && !blockedForCase(lc.name))
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
                    if ((!options.ignoreHidden || !isHiddenName(lc.name)) && !blockedForCase(lc.name))
                    {
                        if (auto reason = blockReasonOf(lc); reason)
                            sink.onBlocked(joinRel(parentRelKey, lc.name), *reason);
                        else if (auto action = chooseLocalOnlyAction(lc, options); action)
                        {
                            const auto relKey = joinRel(parentRelKey, lc.name);
                            emitOneSided(
                                makeOneSidedRow(lc, relKey, *action, true), *action, parentRelKey, options, sink
                            );
                        }
                    }
                    ++li;
                    bumpAndCheckpoint();
                }
                else if (takeRight)
                {
                    auto const& rc = remoteChildren[ri];
                    if ((!options.ignoreHidden || !isHiddenName(rc.name)) && !blockedForCase(rc.name))
                    {
                        if (auto reason = blockReasonOf(rc); reason)
                            sink.onBlocked(joinRel(parentRelKey, rc.name), *reason);
                        else if (auto action = chooseRemoteOnlyAction(rc, options); action)
                        {
                            const auto relKey = joinRel(parentRelKey, rc.name);
                            emitOneSided(
                                makeOneSidedRow(rc, relKey, *action, false), *action, parentRelKey, options, sink
                            );
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

    DiffTreeNode makeOneSidedRow(ScanNode const& node, std::string const& relKey, Action action, bool isLocalSide)
    {
        const bool isDirectory = node.type == FileType::Directory;
        return DiffTreeNode{
            .relKey = relKey,
            .name = node.name,
            .action = action,
            .isDirectory = isDirectory,
            .hasLocalSide = isLocalSide,
            .hasRemoteSide = !isLocalSide,
            .localSize = isLocalSide ? node.size : 0u,
            .remoteSize = isLocalSide ? 0u : node.size,
            .localMtime = isLocalSide ? node.mtime : 0u,
            .remoteMtime = isLocalSide ? 0u : node.mtime,
            .directChildCount = isDirectory ? static_cast<std::uint32_t>(node.children.size()) : 0u,
            .descendantItemCount = isDirectory ? node.subtreeFileCount : 1u,
            .descendantByteTotal = isDirectory ? node.subtreeByteTotal : node.size,
        };
    }

    bool isHiddenName(std::string_view name)
    {
        return !name.empty() && name.front() == '.';
    }

    /**
     * @brief Link literals compare with repeated separators collapsed, "a//b" points where "a/b" does.
     */
    bool linkTargetsDiffer(std::string_view local, std::string_view remote)
    {
        const auto collapsed = [](std::string_view text)
        {
            std::string result;
            result.reserve(text.size());
            for (auto const character : text)
            {
                if (character == '/' && !result.empty() && result.back() == '/')
                    continue;
                result.push_back(character);
            }
            return result;
        };
        return collapsed(local) != collapsed(remote);
    }

    bool entriesDiffer(ScanNode const& localNode, ScanNode const& remoteNode)
    {
        if (localNode.type != remoteNode.type)
            return true;
        if (localNode.type == FileType::Symlink)
        {
            if (localNode.linkTarget && remoteNode.linkTarget)
                return linkTargetsDiffer(*localNode.linkTarget, *remoteNode.linkTarget);
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

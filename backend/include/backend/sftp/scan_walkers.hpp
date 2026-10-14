#pragma once

#include <backend/sftp/operation.hpp>
#include <shared_data/directory_entry.hpp>
#include <shared_data/sync/tree_directory_walker.hpp>
#include <utility/directory_traversal.hpp>
#include <paths/relative_path.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief The walkers the remote and the local scan share: one walker per scan, created on first use as a flat
 *        DeepDirectoryWalker or a sync TreeDirectoryWalker, both listing through the scan's scanner.
 */
namespace ScanWalkers
{
    /**
     * @brief Lists a directory through @p OperationT::scanner. A named type, so every use of a scan's walker names
     *        the same walker type.
     */
    template <typename OperationT>
    struct ScannerOf
    {
        OperationT* operation;

        auto operator()(Paths::RelativePath const& relative) const
        {
            return operation->scanner(relative);
        }
    };

    template <typename OperationT>
    using FlatWalker =
        Utility::DeepDirectoryWalker<SharedData::DirectoryEntry, Operation::Error, ScannerOf<OperationT>>;

    template <typename OperationT>
    using TreeWalker = SharedData::Sync::TreeDirectoryWalker<Operation::Error, ScannerOf<OperationT>>;

    /**
     * @brief Calls @p function with the flat walker of @p operation in @p walker, which it creates when there is none.
     */
    template <typename OperationT, typename FunctionT>
    decltype(auto)
    withFlatWalkerDo(OperationT& operation, std::unique_ptr<Utility::BaseDirectoryWalker>& walker, FunctionT&& function)
    {
        if (!walker)
            walker = std::make_unique<FlatWalker<OperationT>>(ScannerOf<OperationT>{&operation});
        return std::forward<FunctionT>(function)(static_cast<FlatWalker<OperationT>&>(*walker));
    }

    /**
     * @brief Calls @p function with the tree walker of @p operation in @p walker, which it creates when there is
     *        none. A directory below the root that cannot be listed stays in the tree as unreadable.
     *
     * @param requireLocalNames Names must be creatable on this machine, for remote trees.
     */
    template <typename OperationT, typename FunctionT>
    decltype(auto) withTreeWalkerDo(
        OperationT& operation,
        std::unique_ptr<Utility::BaseDirectoryWalker>& walker,
        bool recursive,
        bool requireLocalNames,
        std::vector<std::string> const& excludedSuffixes,
        FunctionT&& function
    )
    {
        if (!walker)
        {
            walker = std::make_unique<TreeWalker<OperationT>>(
                ScannerOf<OperationT>{&operation},
                recursive,
                [](Operation::Error const& error)
                {
                    return error.type == Operation::ErrorType::DirectoryUnreadable;
                },
                requireLocalNames,
                excludedSuffixes
            );
        }
        return std::forward<FunctionT>(function)(static_cast<TreeWalker<OperationT>&>(*walker));
    }

    /**
     * @brief The entries a flat walk refused for their name, below @p root by their names as listed, for display.
     */
    template <typename WalkerT>
    std::vector<std::string> rejectedPaths(WalkerT const& walker, std::string_view root)
    {
        std::vector<std::string> paths;
        paths.reserve(walker.rejectedEntries().size());
        for (auto const& entry : walker.rejectedEntries())
            paths.push_back(Utility::describeWalkedEntry(root, walker.entries(), entry));
        return paths;
    }
}

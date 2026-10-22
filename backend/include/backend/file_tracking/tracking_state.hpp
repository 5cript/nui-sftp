#pragma once

#include <backend/file_tracking/event_spelling.hpp>
#include <paths/paths.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Size and modification time (seconds) of a file on the server.
     */
    struct ServerState
    {
        std::uint64_t mtime{0};
        std::uint64_t size{0};

        friend bool operator==(ServerState const&, ServerState const&) = default;
    };

    /**
     * @brief Whether the server's copy of a file changed since @p known was taken. A file the server has but nobody
     *        saw here, a vanished one and one with another size or time all changed.
     * @param known What the server had when it was last seen, nothing when it had no file there.
     * @param current What the server has now, nothing when it has no file there.
     */
    inline bool serverChanged(std::optional<ServerState> const& known, std::optional<ServerState> const& current)
    {
        if (!known)
            return current.has_value();
        return !current || *current != *known;
    }

    /**
     * @brief The remote path a change at @p changed maps to: @p item stands for @p remotePath, whatever either is
     *        called, a change outside it maps to nothing.
     */
    inline std::expected<Paths::RemotePath, Paths::PathError> remotePathOfChange(
        Paths::LocalPath const& changed,
        Paths::LocalPath const& item,
        Paths::RemotePath const& remotePath
    )
    {
        return changed.relativeTo(item).transform(
            [&remotePath](Paths::RelativePath const& relative)
            {
                return remotePath / relative;
            }
        );
    }

    /**
     * @brief What a move inside a tracked item does on the server.
     */
    enum class MoveMeaning
    {
        /** @brief The server's copy of the source is renamed. */
        Rename,
        /**
         * @brief The destination's content changed and the source is gone: an editor saving through a temporary
         *        file, or a move over an existing file. A rename on the server would fail on the existing
         *        destination or find no source.
         */
        ReplaceDestination,
    };

    /**
     * @brief What a move means when the server has the destination (@p destinationOnServer) or the source
     *        (@p sourceOnServer), as far as file tracking knows.
     */
    inline MoveMeaning meaningOfMove(bool destinationOnServer, bool sourceOnServer)
    {
        return destinationOnServer || !sourceOnServer ? MoveMeaning::ReplaceDestination : MoveMeaning::Rename;
    }

    /**
     * @brief @p path moved along with @p from to @p to, nothing when it is not at or below @p from.
     */
    inline std::optional<Paths::LocalPath>
    movedPath(Paths::LocalPath const& path, Paths::LocalPath const& from, Paths::LocalPath const& to)
    {
        const auto relative = path.relativeTo(from);
        if (!relative)
            return std::nullopt;
        if (relative->empty())
            return to;
        return to.join(*relative)
            .transform(
                [](Paths::LocalPath moved)
                {
                    return std::optional<Paths::LocalPath>{std::move(moved)};
                }
            )
            .value_or(std::nullopt);
    }

    /**
     * @brief Orders local paths by their text, so the paths below one are found by the text they start with.
     */
    struct LocalPathText
    {
        using is_transparent = void;

        static std::string_view textOf(Paths::LocalPath const& path)
        {
            return path.toUtf8();
        }

        static std::string_view textOf(std::string_view text)
        {
            return text;
        }

        bool operator()(auto const& first, auto const& second) const
        {
            return textOf(first) < textOf(second);
        }
    };

    /**
     * @brief What the server is known to have of one tracked item, by local path: what the download brought and
     *        what was uploaded since. Lookups below a path cost a search, not a scan.
     */
    class KnownServerContent
    {
      public:
        void rememberFile(Paths::LocalPath const& path, ServerState state)
        {
            files_.insert_or_assign(path, state);
        }

        void rememberDirectory(Paths::LocalPath const& path)
        {
            directories_.insert(path);
        }

        /**
         * @brief The known state of the file at @p path, nothing when no file is known there.
         */
        std::optional<ServerState> fileState(Paths::LocalPath const& path) const
        {
            const auto found = files_.find(path);
            if (found == files_.end())
                return std::nullopt;
            return found->second;
        }

        /**
         * @brief Whether the server is known to have @p path or anything below it.
         */
        bool has(Paths::LocalPath const& path) const
        {
            return files_.contains(path) || directories_.contains(path) || hasBelow(path);
        }

        /**
         * @brief The known path that names @p path on a file system that ignores Unicode normalization (macOS):
         *        @p path when it is known, else the one known path the watcher spells alike, see eventSpelling. Nothing
         *        when none or several do. Elsewhere only @p path itself.
         */
        std::optional<Paths::LocalPath> knownSpellingOf(Paths::LocalPath const& path) const
        {
            if (files_.contains(path) || directories_.contains(path))
                return path;
            const auto utf8 = path.toUtf8();
            // A spelling that differs in normalization only has a byte outside ASCII, and so has its match.
            const auto firstOutsideAscii = std::ranges::find_if(utf8, isOutsideAscii);
            if (firstOutsideAscii == utf8.end())
                return std::nullopt;
            // Every spelling agrees up to the letter ahead of it, which a combining mark may join.
            const auto agreed = std::max(firstOutsideAscii - utf8.begin() - 1, std::ptrdiff_t{0});
            const auto prefix = std::string_view{utf8}.substr(0, static_cast<std::size_t>(agreed));
            const auto spelling = eventSpelling(utf8);
            std::optional<Paths::LocalPath> found{};
            bool ambiguous{false};
            const auto consider = [&](Paths::LocalPath const& known)
            {
                const auto knownUtf8 = known.toUtf8();
                if (std::ranges::none_of(knownUtf8, isOutsideAscii) || eventSpelling(knownUtf8) != spelling)
                    return;
                ambiguous = ambiguous || found.has_value();
                found = known;
            };
            for (auto const& file : startingWith(files_, prefix))
                consider(pathOf(file));
            for (auto const& directory : startingWith(directories_, prefix))
                consider(pathOf(directory));
            if (ambiguous)
                return std::nullopt;
            return found;
        }

        /**
         * @brief Whether the server is known to have anything below @p path.
         */
        bool hasBelow(Paths::LocalPath const& path) const
        {
            return !belowOf(files_, path).empty() || !belowOf(directories_, path).empty();
        }

        bool hasDirectory(Paths::LocalPath const& path) const
        {
            return directories_.contains(path);
        }

        /**
         * @brief Forgets @p path and everything below it.
         */
        void forget(Paths::LocalPath const& path)
        {
            files_.erase(path);
            directories_.erase(path);
            kept_.erase(path);
            eraseBelow(files_, path);
            eraseBelow(directories_, path);
            eraseBelow(kept_, path);
        }

        /**
         * @brief Moves what is known at @p from and below it to @p to, after the server renamed it.
         */
        void move(Paths::LocalPath const& from, Paths::LocalPath const& to)
        {
            moveIn(files_, from, to);
            moveIn(directories_, from, to);
            moveIn(kept_, from, to);
        }

        /**
         * @brief The directory @p path left locally with its content, its server copy stays (see keptDirectories).
         */
        void keep(Paths::LocalPath const& path)
        {
            kept_.insert(path);
        }

        /**
         * @brief The directories that left with their content while the server kept its copies: moved to the trash,
         *        out of the item, or within it as a removal and an addition (see holdsAllBelow).
         */
        std::vector<Paths::LocalPath> keptDirectories() const
        {
            return {kept_.begin(), kept_.end()};
        }

        /**
         * @brief The kept directories above @p path that nothing is known below anymore, closest first: their
         *        content was removed after they were.
         */
        std::vector<Paths::LocalPath> emptiedKeptAbove(Paths::LocalPath const& path) const
        {
            std::vector<Paths::LocalPath> emptied;
            for (auto parent = path.parent(); parent && !hasBelow(*parent); parent = parent->parent())
                if (kept_.contains(*parent))
                    emptied.push_back(*parent);
            return emptied;
        }

        /**
         * @brief Whether everything known below @p original is known below @p copy too, at the same relative path
         *        and with the same state. Moves carry the modification times along.
         */
        bool holdsAllBelow(Paths::LocalPath const& copy, Paths::LocalPath const& original) const
        {
            const auto holdsFile = [this, &copy, &original](auto const& file)
            {
                const auto relative = file.first.relativeTo(original);
                if (!relative)
                    return false;
                const auto atCopy = copy.join(*relative);
                return atCopy && fileState(*atCopy) == file.second;
            };
            const auto holdsDirectory = [this, &copy, &original](Paths::LocalPath const& directory)
            {
                const auto relative = directory.relativeTo(original);
                if (!relative)
                    return false;
                const auto atCopy = copy.join(*relative);
                return atCopy && directories_.contains(*atCopy);
            };
            const auto files = belowOf(files_, original);
            const auto directories = belowOf(directories_, original);
            return (!files.empty() || !directories.empty()) && std::ranges::all_of(files, holdsFile) &&
                std::ranges::all_of(directories, holdsDirectory);
        }

        /**
         * @brief What is known at and below @p path, deepest first: the order to remove it entry by entry.
         */
        std::vector<Paths::LocalPath> deepestFirst(Paths::LocalPath const& path) const
        {
            std::vector<Paths::LocalPath> paths;
            if (files_.contains(path) || directories_.contains(path))
                paths.push_back(path);
            for (auto const& file : belowOf(files_, path))
                paths.push_back(file.first);
            for (auto const& directory : belowOf(directories_, path))
                paths.push_back(directory);
            // A path sorts before everything below it.
            std::ranges::sort(paths, std::ranges::greater{}, &Paths::LocalPath::toUtf8);
            return paths;
        }

      private:
        static bool isOutsideAscii(char character)
        {
            return static_cast<unsigned char>(character) >= 0x80u;
        }

        static Paths::LocalPath const& pathOf(std::pair<Paths::LocalPath const, ServerState> const& file)
        {
            return file.first;
        }

        static Paths::LocalPath const& pathOf(Paths::LocalPath const& directory)
        {
            return directory;
        }

        /**
         * @brief The entries of @p container whose path starts with @p prefix.
         */
        template <typename ContainerT>
        static std::ranges::subrange<decltype(std::declval<ContainerT&>().begin())>
        startingWith(ContainerT& container, std::string_view prefix)
        {
            const auto first = container.lower_bound(prefix);
            auto last = first;
            while (last != container.end() && pathOf(*last).toUtf8().starts_with(prefix))
                ++last;
            return std::ranges::subrange{first, last};
        }

        /**
         * @brief The entries of @p container strictly below @p path.
         */
        template <typename ContainerT>
        static std::ranges::subrange<decltype(std::declval<ContainerT&>().begin())>
        belowOf(ContainerT& container, Paths::LocalPath const& path)
        {
            const auto prefix = path.isRoot() ? path.toUtf8() : path.toUtf8() + '/';
            auto below = startingWith(container, prefix);
            // A root ends with its separator, it is its own first match.
            if (!below.empty() && pathOf(*below.begin()) == path)
                below.advance(1);
            return below;
        }

        template <typename ContainerT>
        static void eraseBelow(ContainerT& container, Paths::LocalPath const& path)
        {
            const auto below = belowOf(container, path);
            container.erase(below.begin(), below.end());
        }

        /**
         * @brief Moves the entries at and below @p from in @p container to @p to, they replace what is known there.
         */
        template <typename ContainerT>
        static void moveIn(ContainerT& container, Paths::LocalPath const& from, Paths::LocalPath const& to)
        {
            std::vector<typename ContainerT::node_type> nodes;
            if (auto node = container.extract(from))
                nodes.push_back(std::move(node));
            const auto below = belowOf(container, from);
            for (auto entry = below.begin(); entry != below.end();)
                nodes.push_back(container.extract(entry++));
            for (auto& node : nodes)
            {
                auto& path = [&node]() -> Paths::LocalPath&
                {
                    if constexpr (requires { node.key(); })
                        return node.key();
                    else
                        return node.value();
                }();
                path = movedPath(path, from, to).value_or(path);
                container.erase(path);
                container.insert(std::move(node));
            }
        }

      private:
        std::map<Paths::LocalPath, ServerState, LocalPathText> files_;
        std::set<Paths::LocalPath, LocalPathText> directories_;
        std::set<Paths::LocalPath, LocalPathText> kept_;
    };

    /**
     * @brief Keeps the server operations of a tracker in the order it decided them: one waits until every earlier
     *        one on the same path, a path above it or one below it ended. The operation queue runs operations side by
     *        side, a delete could otherwise remove the upload that followed it, or a directory's delete come before
     *        the deletes of its content.
     */
    class ServerOperationOrder
    {
      public:
        using Ticket = std::uint64_t;

        /**
         * @brief Registers an operation on @p paths.
         * @return Its ticket and whether it may start now. One that waits starts once leave() returns its ticket.
         */
        std::pair<Ticket, bool> enter(std::vector<Paths::RemotePath> paths)
        {
            const auto ticket = nextTicket_++;
            const bool startsNow = std::ranges::none_of(
                operations_,
                [&paths](Operation const& earlier)
                {
                    return overlaps(earlier.paths, paths);
                }
            );
            operations_.push_back({.ticket = ticket, .paths = std::move(paths), .started = startsNow});
            return {ticket, startsNow};
        }

        /**
         * @brief The operation @p ticket ended, or never starts.
         * @return The tickets that may start now, in the order they entered.
         */
        std::vector<Ticket> leave(Ticket ticket)
        {
            const auto left = std::ranges::find(operations_, ticket, &Operation::ticket);
            if (left == operations_.end())
                return {};
            const auto leftPaths = std::move(left->paths);
            operations_.erase(left);
            std::vector<Ticket> starting;
            for (auto waiting = operations_.begin(); waiting != operations_.end(); ++waiting)
            {
                if (waiting->started || !overlaps(waiting->paths, leftPaths))
                    continue;
                const bool blocked = std::ranges::any_of(
                    operations_.begin(),
                    waiting,
                    [&waiting](Operation const& earlier)
                    {
                        return overlaps(earlier.paths, waiting->paths);
                    }
                );
                if (blocked)
                    continue;
                waiting->started = true;
                starting.push_back(waiting->ticket);
            }
            return starting;
        }

        bool empty() const
        {
            return operations_.empty();
        }

      private:
        struct Operation
        {
            Ticket ticket;
            std::vector<Paths::RemotePath> paths;
            bool started;
        };

        static bool overlaps(std::vector<Paths::RemotePath> const& first, std::vector<Paths::RemotePath> const& second)
        {
            return std::ranges::any_of(
                first,
                [&second](Paths::RemotePath const& one)
                {
                    return std::ranges::any_of(
                        second,
                        [&one](Paths::RemotePath const& other)
                        {
                            return one.isWithin(other) || other.isWithin(one);
                        }
                    );
                }
            );
        }

      private:
        std::deque<Operation> operations_{};
        Ticket nextTicket_{0};
    };

    /**
     * @brief A changed path of a tracked item.
     */
    struct LocalChange
    {
        Paths::LocalPath path;
        bool isDirectory{false};

        friend bool operator==(LocalChange const&, LocalChange const&) = default;
    };

    /**
     * @brief The uploads of one tracked item that are on their way, from the change until the server's state after
     *        the upload is known. One path has at most one, and nothing below a directory being uploaded starts its
     *        own, so two uploads never write the same remote file at once.
     */
    class PendingUploads
    {
      public:
        /**
         * @brief What became of the uploads of a path when its upload ended.
         */
        struct Finished
        {
            /** @brief The path changed again meanwhile and has to go once more. */
            bool changedAgain{false};
            /** @brief The path was deleted or moved away meanwhile, its server copy has to go. */
            bool removeRemote{false};
            /** @brief Whether the path was a directory at its latest change. */
            bool isDirectory{false};
            /** @brief Changes below the uploaded directory that arrived meanwhile, in arrival order. */
            std::vector<LocalChange> changedBelow{};
            /** @brief Where the path moved meanwhile, see moveWhenDone and moveBelow, nothing when it stayed. */
            std::optional<Paths::LocalPath> movedTo{};
            /**
             * @brief Where its server copy is now by local path, when a directory above took it along (see
             *        moveBelow), nothing when it is where the upload started.
             */
            std::optional<Paths::LocalPath> serverCopyAt{};
        };

        /**
         * @brief Starts the upload of @p change unless one of its path or of a directory above it is on its way, the
         *        change then goes again once that one finished, as the kind it has now (see Finished::isDirectory).
         * @return Whether the upload starts.
         */
        bool start(LocalChange const& change)
        {
            if (const auto found = std::ranges::find_if(pending_, IsAt{change.path}); found != pending_.end())
            {
                found->second.changedAgain = true;
                found->second.removeRemote = false;
                found->second.isDirectory = change.isDirectory;
                return false;
            }
            const auto directoryAbove = std::ranges::find_if(
                pending_,
                [&change](auto const& pending)
                {
                    return pending.second.isDirectory && change.path.isWithin(currentPath(pending));
                }
            );
            if (directoryAbove != pending_.end())
            {
                if (!std::ranges::contains(directoryAbove->second.changedBelow, change))
                    directoryAbove->second.changedBelow.push_back(change);
                return false;
            }
            pending_.emplace(change.path, Pending{.isDirectory = change.isDirectory});
            return true;
        }

        /**
         * @brief The content of @p path as it is now is being handled, by the upload reading it or by the answer to
         *        a question about it: the changes that arrived so far need nothing more.
         */
        void changesHandled(Paths::LocalPath const& path)
        {
            if (const auto found = pending_.find(path); found != pending_.end())
            {
                found->second.changedAgain = false;
                found->second.changedBelow.clear();
            }
        }

        /**
         * @brief Whether an upload of @p path is on its way.
         */
        bool isPending(Paths::LocalPath const& path) const
        {
            return std::ranges::any_of(pending_, IsAt{path});
        }

        /**
         * @brief The path of a pending upload was deleted or moved away: its server copy goes once the upload
         *        ended, and it does not go again.
         * @return Whether an upload of @p path is on its way.
         */
        bool removeRemoteWhenDone(Paths::LocalPath const& path)
        {
            const auto found = std::ranges::find_if(pending_, IsAt{path});
            if (found == pending_.end())
                return false;
            found->second.removeRemote = true;
            found->second.changedAgain = false;
            return true;
        }

        /**
         * @brief The path of a pending upload moved from @p from to @p to: its server copy follows once the upload
         *        ended (see Finished::movedTo), the changes at and below @p to wait for it like those of the path did.
         * @return Whether an upload of @p from is on its way.
         */
        bool moveWhenDone(Paths::LocalPath const& from, Paths::LocalPath const& to)
        {
            const auto found = std::ranges::find_if(pending_, IsAt{from});
            if (found == pending_.end())
                return false;
            found->second.movedTo = to;
            for (auto& change : found->second.changedBelow)
                change.path = movedPath(change.path, from, to).value_or(change.path);
            return true;
        }

        /**
         * @brief A directory above pending uploads moved from @p from to @p to: they read the paths below @p to now,
         *        see Finished::movedTo, and so do the changes waiting for them.
         * @param serverCopiesFollow Whether the server copies below @p from are now those below @p to, because the
         *        server's directory was renamed too or the item that maps to the server was.
         */
        void moveBelow(Paths::LocalPath const& from, Paths::LocalPath const& to, bool serverCopiesFollow)
        {
            const auto movedBelow = [&from, &to](Paths::LocalPath const& path) -> std::optional<Paths::LocalPath>
            {
                if (path == from)
                    return std::nullopt;
                return movedPath(path, from, to);
            };
            for (auto& [path, pending] : pending_)
            {
                if (const auto local = movedBelow(currentPathOf(path, pending)))
                    pending.movedTo = *local;
                if (const auto server = movedBelow(pending.serverCopyAt.value_or(path)); server && serverCopiesFollow)
                    pending.serverCopyAt = *server;
                for (auto& change : pending.changedBelow)
                    change.path = movedBelow(change.path).value_or(change.path);
            }
        }

        /**
         * @brief Whether the path of the pending upload that started at @p path is no longer there, see moveWhenDone
         *        and moveBelow: reading it would fail.
         */
        bool movedAway(Paths::LocalPath const& path) const
        {
            const auto found = pending_.find(path);
            return found != pending_.end() && found->second.movedTo.has_value();
        }

        /**
         * @brief Ends the upload of @p path, the path it started with.
         */
        Finished finish(Paths::LocalPath const& path)
        {
            const auto found = pending_.find(path);
            if (found == pending_.end())
                return {};
            auto finished = Finished{
                .changedAgain = found->second.changedAgain,
                .removeRemote = found->second.removeRemote,
                .isDirectory = found->second.isDirectory,
                .changedBelow = std::move(found->second.changedBelow),
                .movedTo = std::move(found->second.movedTo),
                .serverCopyAt = std::move(found->second.serverCopyAt),
            };
            pending_.erase(found);
            return finished;
        }

        bool empty() const
        {
            return pending_.empty();
        }

        /**
         * @brief The changes on their way: each pending upload, the one more it takes when its path changed again and
         *        the changes waiting below a directory.
         */
        std::size_t changeCount() const
        {
            std::size_t count = 0;
            for (auto const& pending : std::views::values(pending_))
                count += 1 + (pending.changedAgain ? 1 : 0) + pending.changedBelow.size();
            return count;
        }

      private:
        struct Pending
        {
            bool isDirectory{false};
            bool changedAgain{false};
            bool removeRemote{false};
            std::vector<LocalChange> changedBelow{};
            std::optional<Paths::LocalPath> movedTo{};
            std::optional<Paths::LocalPath> serverCopyAt{};
        };

        static Paths::LocalPath const& currentPathOf(Paths::LocalPath const& path, Pending const& pending)
        {
            return pending.movedTo ? *pending.movedTo : path;
        }

        /**
         * @brief Where the path of @p pending is now, it is keyed by the one its upload started with.
         */
        static Paths::LocalPath const& currentPath(std::pair<Paths::LocalPath const, Pending> const& pending)
        {
            return currentPathOf(pending.first, pending.second);
        }

        /**
         * @brief Matches the pending upload whose path is @p path now.
         */
        struct IsAt
        {
            Paths::LocalPath const& path;

            bool operator()(std::pair<Paths::LocalPath const, Pending> const& pending) const
            {
                return currentPath(pending) == path;
            }
        };

        std::map<Paths::LocalPath, Pending> pending_;
    };
}

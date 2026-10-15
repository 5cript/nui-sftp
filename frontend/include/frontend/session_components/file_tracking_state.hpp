#pragma once

#include <paths/paths.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
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
     * @brief The remote path a change at @p changed maps to. The instance directory stands for the directory of
     *        @p remotePath, a change outside it or of the instance directory itself maps to nothing.
     */
    inline std::expected<Paths::RemotePath, Paths::PathError> remotePathOfChange(
        Paths::LocalPath const& changed,
        Paths::LocalPath const& instanceDirectory,
        Paths::RemotePath const& remotePath
    )
    {
        const auto remoteDirectory = remotePath.parent();
        if (!remoteDirectory)
            return std::unexpected(
                Paths::PathError{.reason = Paths::PathRejection::EscapesRoot, .input = remotePath.value()}
            );
        return changed.relativeTo(instanceDirectory)
            .and_then(
                [&changed, &remoteDirectory](
                    Paths::RelativePath const& relative
                ) -> std::expected<Paths::RemotePath, Paths::PathError>
                {
                    if (relative.empty())
                        return std::unexpected(
                            Paths::PathError{.reason = Paths::PathRejection::Empty, .input = changed.toUtf8()}
                        );
                    return *remoteDirectory / relative;
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
     * @brief What the server is known to have of one tracked item, by local path: what the download brought and
     *        what was uploaded since.
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
         * @brief Whether the server is known to have anything below @p path.
         */
        bool hasBelow(Paths::LocalPath const& path) const
        {
            const auto isBelow = [&path](Paths::LocalPath const& candidate)
            {
                return candidate != path && candidate.isWithin(path);
            };
            return std::ranges::any_of(std::views::keys(files_), isBelow) || std::ranges::any_of(directories_, isBelow);
        }

        /**
         * @brief Forgets @p path and everything below it.
         */
        void forget(Paths::LocalPath const& path)
        {
            std::erase_if(
                files_,
                [&path](auto const& file)
                {
                    return file.first.isWithin(path);
                }
            );
            std::erase_if(
                directories_,
                [&path](Paths::LocalPath const& directory)
                {
                    return directory.isWithin(path);
                }
            );
        }

        /**
         * @brief Moves what is known at @p from and below it to @p to, after the server renamed it.
         */
        void move(Paths::LocalPath const& from, Paths::LocalPath const& to)
        {
            const auto moved = [&from, &to](Paths::LocalPath const& path) -> std::optional<Paths::LocalPath>
            {
                return path.relativeTo(from)
                    .and_then(
                        [&to](Paths::RelativePath const& relative)
                        {
                            return to.join(relative);
                        }
                    )
                    .transform(
                        [](Paths::LocalPath moved)
                        {
                            return std::optional<Paths::LocalPath>{std::move(moved)};
                        }
                    )
                    .value_or(std::nullopt);
            };
            std::map<Paths::LocalPath, ServerState> files;
            for (auto const& [path, state] : files_)
                files.insert_or_assign(moved(path).value_or(path), state);
            std::set<Paths::LocalPath> directories;
            std::ranges::transform(
                directories_,
                std::inserter(directories, directories.end()),
                [&moved](Paths::LocalPath const& path)
                {
                    return moved(path).value_or(path);
                }
            );
            files_ = std::move(files);
            directories_ = std::move(directories);
        }

      private:
        std::map<Paths::LocalPath, ServerState> files_;
        std::set<Paths::LocalPath> directories_;
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
            /** @brief Changes below the uploaded directory that arrived meanwhile, in arrival order. */
            std::vector<LocalChange> changedBelow{};
        };

        /**
         * @brief Starts the upload of @p change unless one of its path or of a directory above it is on its way, the
         *        change then goes again once that one finished.
         * @return Whether the upload starts.
         */
        bool start(LocalChange const& change)
        {
            if (const auto found = pending_.find(change.path); found != pending_.end())
            {
                found->second.changedAgain = true;
                found->second.removeRemote = false;
                return false;
            }
            const auto directoryAbove = std::ranges::find_if(
                pending_,
                [&change](auto const& pending)
                {
                    return pending.second.isDirectory && change.path.isWithin(pending.first);
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
            return pending_.contains(path);
        }

        /**
         * @brief The path of a pending upload was deleted or moved away: its server copy goes once the upload
         *        ended, and it does not go again.
         * @return Whether an upload of @p path is on its way.
         */
        bool removeRemoteWhenDone(Paths::LocalPath const& path)
        {
            const auto found = pending_.find(path);
            if (found == pending_.end())
                return false;
            found->second.removeRemote = true;
            found->second.changedAgain = false;
            return true;
        }

        /**
         * @brief Ends the upload of @p path.
         */
        Finished finish(Paths::LocalPath const& path)
        {
            const auto found = pending_.find(path);
            if (found == pending_.end())
                return {};
            auto finished = Finished{
                .changedAgain = found->second.changedAgain,
                .removeRemote = found->second.removeRemote,
                .changedBelow = std::move(found->second.changedBelow),
            };
            pending_.erase(found);
            return finished;
        }

        bool empty() const
        {
            return pending_.empty();
        }

      private:
        struct Pending
        {
            bool isDirectory{false};
            bool changedAgain{false};
            bool removeRemote{false};
            std::vector<LocalChange> changedBelow{};
        };
        std::map<Paths::LocalPath, Pending> pending_;
    };

    /**
     * @brief The keys of a table's rows in the order they were added, so the row of a key can be found.
     */
    template <typename KeyT>
    class RowOrder
    {
      public:
        /**
         * @brief Appends the row of @p key.
         */
        void add(KeyT key)
        {
            keys_.push_back(std::move(key));
        }

        /**
         * @brief Forgets the row of @p key.
         * @return The index the row had, nothing when @p key has no row.
         */
        std::optional<std::size_t> remove(KeyT const& key)
        {
            const auto found = std::ranges::find(keys_, key);
            if (found == keys_.end())
                return std::nullopt;
            const auto index = static_cast<std::size_t>(std::distance(keys_.begin(), found));
            keys_.erase(found);
            return index;
        }

        void clear()
        {
            keys_.clear();
        }

        std::size_t size() const
        {
            return keys_.size();
        }

      private:
        std::vector<KeyT> keys_{};
    };
}

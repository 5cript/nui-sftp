#include <ftp/remote/file_system_operations.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/this_coro.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using RemoteIo::RemoteErrorKind;
        using SharedData::FileType;

        /** @brief The links one lookup follows before their chain counts as endless. */
        constexpr int maximumLinks = 10;

        /** @brief The entries of a listing by their names, of which a Listing keeps one each. */
        using ListedNames = std::unordered_map<std::string_view, ListingEntry const*>;

        RemoteIo::RemoteError cancelledError()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The operation was cancelled");
        }

        void reportLeftOut(Listing const& listing)
        {
            if (listing.unreadableLines == 0u && listing.refusedNames == 0u && listing.droppedEntries == 0u)
                return;
            Log::info(
                "FTP: A listing left out {} unreadable lines, {} entries with unusable names and {} past the limit",
                listing.unreadableLines,
                listing.refusedNames,
                listing.droppedEntries
            );
        }

        /**
         * @brief The listing has no entry and nothing the parser left out.
         */
        bool listsNothing(Listing const& listing)
        {
            return listing.entries.empty() && listing.unreadableLines == 0u && listing.refusedNames == 0u &&
                listing.droppedEntries == 0u;
        }

        /**
         * @brief The lookup failed because of what is at the path, not because the session cannot ask.
         */
        bool aboutThePath(RemoteIo::RemoteError const& error)
        {
            return RemoteIo::isMissing(error) || error.kind == RemoteErrorKind::PermissionDenied ||
                error.kind == RemoteErrorKind::Failure;
        }

        RemoteIo::FileInformation informationOf(ListingEntry const& entry)
        {
            return toDirectoryEntry(entry);
        }

        std::expected<Paths::RemotePath, RemoteIo::RemoteError> remotePathOf(std::string const& path)
        {
            return Paths::RemotePath::make(path).transform_error(
                [](Paths::PathError const& error)
                {
                    return errorOf(
                        RemoteErrorKind::ProtocolViolation,
                        fmt::format("The server named an unusable path: {}", error.toString())
                    );
                }
            );
        }

        /**
         * @brief Where the symlink @p entry at @p path points to.
         */
        std::expected<Paths::RemotePath, RemoteIo::RemoteError>
        targetOf(Paths::RemotePath const& path, RemoteIo::FileInformation const& entry)
        {
            if (!entry.linkTarget)
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "The listing names no link target"));
            return path.resolveLink(*entry.linkTarget)
                .transform_error(
                    [&path](Paths::PathError const& error)
                    {
                        return errorOf(
                            RemoteErrorKind::Failure,
                            fmt::format(
                                "Symlink '{}' has an unusable target: {}", path.value(), error.toString()
                            )
                        );
                    }
                );
        }

        /**
         * @brief The entries that can answer where a link of @p listing leads to. None where the server listed a name
         *        more than once: the listing kept one of them, which need not be the one the link reaches.
         */
        ListedNames listedByName(Listing const& listing)
        {
            ListedNames names{};
            if (listing.repeatedNames != 0u)
                return names;
            names.reserve(listing.entries.size());
            for (auto const& entry : listing.entries)
                names.emplace(entry.name, &entry);
            return names;
        }

        /**
         * @brief The entry that the symlink @p link ends at, where its listing tells: every link of the chain names
         *        one entry of the same listing, and the last of them is a file or a directory. Costs no command.
         *
         * @return Nothing for a target that is a path, "." or "..", that @p names does not have, and for a chain that
         *         does not end: the server has to follow those.
         */
        ListingEntry const* listedTargetOf(ListedNames const& names, ListingEntry const& link)
        {
            auto const* current = &link;
            for (int links = 0; links < maximumLinks && current->linkTarget; ++links)
            {
                std::string_view const target{*current->linkTarget};
                if (target.empty() || target == "." || target == ".." || target.contains('/'))
                    return nullptr;
                const auto found = names.find(target);
                if (found == names.end())
                    return nullptr;
                current = found->second;
                if (current->type == FileType::Regular || current->type == FileType::Directory)
                    return current;
                if (current->type != FileType::Symlink)
                    return nullptr;
            }
            return nullptr;
        }

        /**
         * @brief @p target as the end of the symlink @p link, named like the link, as statLinkTarget() names it.
         */
        std::shared_ptr<SharedData::DirectoryEntry>
        resolvedAs(ListingEntry const& target, RemoteIo::FileInformation const& link)
        {
            auto resolved = std::make_shared<SharedData::DirectoryEntry>(informationOf(target));
            resolved->name = link.name;
            resolved->nameIsValidUtf8 = link.nameIsValidUtf8;
            return resolved;
        }

        /**
         * @brief The end of the symlink @p entry of the listing of @p directory, nothing when it cannot be reached.
         */
        asio::awaitable<std::expected<std::shared_ptr<SharedData::DirectoryEntry>, RemoteIo::RemoteError>>
        resolvedTargetOf(MetadataSession session, Paths::RemotePath const& directory, RemoteIo::FileInformation const& entry)
        {
            // A name that is no valid segment must not become a path to look up.
            const auto name = entry.nameSegment();
            if (!name)
                co_return nullptr;
            // The listing told that it is a link, looking it up again would list its directory once per dead link.
            auto target = co_await statLinkTarget(std::move(session), (directory / *name).value());
            if (target)
                co_return std::make_shared<SharedData::DirectoryEntry>(informationOf(*target));
            if (aboutThePath(target.error()))
                co_return nullptr;
            co_return std::unexpected(std::move(target).error());
        }
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::ListDirectory>>
    runOperation(MetadataSession session, RemoteIo::ListDirectory operation)
    {
        auto listing = co_await listDirectory(session, operation.path.value());
        if (!listing)
            co_return std::unexpected(std::move(listing).error());
        reportLeftOut(*listing);
        std::vector<RemoteIo::FileInformation> entries{};
        entries.reserve(listing->entries.size());
        for (auto const& listed : listing->entries)
            entries.push_back(informationOf(listed));
        if (!operation.resolveLinks || std::ranges::none_of(entries, &RemoteIo::FileInformation::isSymlink))
            co_return entries;
        const auto names = listedByName(*listing);
        for (std::size_t index = 0u; index < entries.size(); ++index)
        {
            auto& entry = entries[index];
            if (!entry.isSymlink())
                continue;
            // A link to an entry of this listing needs no lookup, each one costs commands within the same backstop.
            if (auto const* target = listedTargetOf(names, listing->entries[index]))
            {
                entry.resolvedTarget = resolvedAs(*target, entry);
                continue;
            }
            if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelledError());
            auto target = co_await resolvedTargetOf(session, operation.path, entry);
            if (!target)
                co_return std::unexpected(std::move(target).error());
            entry.resolvedTarget = *std::move(target);
        }
        co_return entries;
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::Stat>>
    runOperation(MetadataSession session, RemoteIo::Stat operation)
    {
        co_return (co_await statPath(std::move(session), operation.path.value())).transform(informationOf);
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::Lstat>>
    runOperation(MetadataSession session, RemoteIo::Lstat operation)
    {
        co_return (co_await lstatPath(std::move(session), operation.path.value())).transform(informationOf);
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateNewDirectory>>
    runOperation(MetadataSession session, RemoteIo::CreateNewDirectory operation)
    {
        co_return co_await makeDirectory(std::move(session), operation.path.value());
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateDirectoryIfItDoesntExist>>
    runOperation(MetadataSession session, RemoteIo::CreateDirectoryIfItDoesntExist operation)
    {
        auto made = co_await makeDirectory(session, operation.path.value());
        if (made || made.error().kind != RemoteErrorKind::AlreadyExists)
            co_return made;
        if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(cancelledError());
        const auto existing = co_await statPath(std::move(session), operation.path.value());
        if (!existing)
            co_return std::unexpected(existing.error());
        if (existing->type != FileType::Directory)
            co_return std::unexpected(errorOf(RemoteErrorKind::AlreadyExists, "The path exists and is no directory"));
        co_return RemoteIo::OperationResult<RemoteIo::CreateDirectoryIfItDoesntExist>{};
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateEmptyFile>>
    runOperation(MetadataSession session, RemoteIo::CreateEmptyFile operation)
    {
        co_return co_await createEmptyFile(std::move(session), operation.path.value());
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveFile>>
    runOperation(MetadataSession session, RemoteIo::RemoveFile operation)
    {
        co_return co_await removeFile(std::move(session), operation.path.value());
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveEmptyDirectory>>
    runOperation(MetadataSession session, RemoteIo::RemoveEmptyDirectory operation)
    {
        co_return co_await removeDirectory(std::move(session), operation.path.value());
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveAll>>
    runOperation(MetadataSession session, RemoteIo::RemoveAll operation)
    {
        for (auto const& path : operation.paths)
        {
            if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelledError());
            if (auto removed = co_await removeEntry(session, path.value()); !removed)
                co_return removed;
        }
        co_return RemoteIo::OperationResult<RemoteIo::RemoveAll>{};
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::FilterOutEmptyDirectories>>
    runOperation(MetadataSession session, RemoteIo::FilterOutEmptyDirectories operation)
    {
        std::vector<Paths::RemotePath> withContent{};
        for (auto const& directory : operation.directories)
        {
            if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelledError());
            auto listing = co_await listDirectory(session, directory.value());
            if (listing)
            {
                if (!listsNothing(*listing))
                    withContent.push_back(directory);
                continue;
            }
            if (!aboutThePath(listing.error()) || Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(std::move(listing).error());
            // Lists of directories are what this costs, a lookup only where the listing failed.
            const auto entry = co_await lstatPath(session, directory.value());
            if (!entry)
                co_return std::unexpected(entry.error());
            if (entry->type == FileType::Directory)
                co_return std::unexpected(std::move(listing).error());
        }
        co_return withContent;
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::Rename>>
    runOperation(MetadataSession session, RemoteIo::Rename operation)
    {
        co_return co_await renamePath(std::move(session), operation.source.value(), operation.destination.value());
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::LinkIntoPlace>>
    runOperation(MetadataSession session, RemoteIo::LinkIntoPlace operation)
    {
        const auto existing = co_await lstatPath(std::move(session), operation.destination.value());
        if (existing)
            co_return std::unexpected(RemoteIo::alreadyExistsError(operation.destination.value()));
        // Unsupported promises a free destination, which only a missing one is.
        if (!RemoteIo::isMissing(existing.error()))
            co_return std::unexpected(existing.error());
        co_return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP has no hard links"));
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadFile>>
    runOperation(MetadataSession session, RemoteIo::ReadFile operation)
    {
        constexpr std::size_t pieceSize = 64u * 1024u;
        auto started =
            co_await Download::start(session.transfer, DownloadRequest{.path = operation.path.value()}, session.options);
        if (!started)
            co_return std::unexpected(std::move(started).error());
        const auto download = *std::move(started);
        std::string content{};
        std::vector<char> piece(std::min(operation.maximumSize, pieceSize));
        while (content.size() < operation.maximumSize)
        {
            const auto wanted = std::min(piece.size(), operation.maximumSize - content.size());
            const auto read = co_await download->readSome(asio::buffer(piece.data(), wanted));
            if (!read)
                co_return std::unexpected(read.error());
            if (*read == 0u)
            {
                const auto finished = co_await download->finish();
                if (!finished)
                    co_return std::unexpected(finished.error());
                co_return content;
            }
            content.append(piece.data(), *read);
        }
        // The rest is not wanted, the connection drains behind the ABOR.
        download->abort();
        co_return content;
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::SetModificationTime>>
    runOperation(MetadataSession session, RemoteIo::SetModificationTime operation)
    {
        co_return co_await setModificationTime(
            std::move(session),
            operation.path.value(),
            std::chrono::floor<std::chrono::seconds>(operation.modificationTime)
        );
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::CanonicalPath>>
    runOperation(MetadataSession session, RemoteIo::CanonicalPath operation)
    {
        auto current = operation.path;
        for (int links = 0; links <= maximumLinks; ++links)
        {
            auto found = co_await canonicalDirectoryOrEntry(session, current.value());
            if (!found)
                co_return std::unexpected(std::move(found).error());
            if (auto const* directory = std::get_if<std::string>(&*found))
                co_return remotePathOf(*directory);
            if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelledError());

            // No directory: named in its resolved parent, from where a link leads on.
            const auto parent = current.parent();
            const auto name = current.filename();
            if (!parent || !name)
                co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The root is no directory"));
            const auto resolvedParent = co_await canonicalDirectory(session, parent->value());
            if (!resolvedParent)
                co_return std::unexpected(resolvedParent.error());
            auto resolved = remotePathOf(*resolvedParent).transform(
                [&name](Paths::RemotePath const& directory)
                {
                    return directory / *name;
                }
            );
            auto const& entry = std::get<ListingEntry>(*found);
            if (!resolved || entry.type != FileType::Symlink)
                co_return resolved;
            auto target = targetOf(*resolved, informationOf(entry));
            if (!target)
                co_return std::unexpected(std::move(target).error());
            current = *std::move(target);
        }
        co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "Maximum symlink depth exceeded"));
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadLinkDeep>>
    runOperation(MetadataSession session, RemoteIo::ReadLinkDeep operation)
    {
        auto current = operation.path;
        std::optional<std::string> linkContent{};
        for (int depth = 0; depth < operation.maxDepth; ++depth)
        {
            if (Detail::isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelledError());
            const auto entry = co_await lstatPath(session, current.value());
            if (!entry)
            {
                if (!RemoteIo::isMissing(entry.error()))
                    co_return std::unexpected(entry.error());
                co_return RemoteIo::DeepLinkResult{
                    .linkContent = linkContent.value_or(""),
                    .linkTarget = current,
                    .targetInfo = std::nullopt,
                };
            }
            auto information = informationOf(*entry);
            if (information.type != FileType::Symlink)
            {
                co_return RemoteIo::DeepLinkResult{
                    .linkContent = linkContent.value_or(""),
                    .linkTarget = current,
                    .targetInfo = std::move(information),
                };
            }
            auto target = targetOf(current, information);
            if (!target)
                co_return std::unexpected(std::move(target).error());
            if (!linkContent)
                linkContent = information.linkTarget;
            current = *std::move(target);
        }
        co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "Maximum symlink depth exceeded"));
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadLink>>
    runOperation(MetadataSession session, RemoteIo::ReadLink operation)
    {
        const auto entry = co_await lstatPath(std::move(session), operation.path.value());
        if (!entry)
            co_return std::unexpected(entry.error());
        auto information = informationOf(*entry);
        if (information.type != FileType::Symlink)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The path is no symbolic link"));
        if (!information.linkTarget)
            co_return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "The listing names no link target"));
        co_return *std::move(information.linkTarget);
    }
}

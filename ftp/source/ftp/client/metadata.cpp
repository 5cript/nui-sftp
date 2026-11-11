#include <ftp/client/metadata.hpp>
#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <paths/utf8.hpp>
#include "client_support.hpp"

#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::succeeded;
        using SharedData::FileType;
        using RemoteIo::RemoteErrorKind;

        void learnOnce(std::atomic<Support>& fact, Support support)
        {
            auto unknown = Support::Unknown;
            fact.compare_exchange_strong(unknown, support);
        }

        /**
         * @brief The server answered that it lacks the command: 500, 502 or 504.
         */
        bool lacksTheCommand(RemoteIo::RemoteError const& error)
        {
            return error.kind == RemoteErrorKind::Unsupported;
        }

        std::chrono::sys_days today()
        {
            return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        }

        asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        changeDirectory(std::shared_ptr<ControlConnection> control, std::string path)
        {
            const auto reply =
                co_await control->execute(Command{.verb = Verb::ChangeWorkingDirectory, .argument = std::move(path)});
            if (!reply)
                co_return std::unexpected(reply.error());
            if (!succeeded(*reply))
                co_return std::unexpected(replyError(*reply, Verb::ChangeWorkingDirectory));
            co_return std::expected<void, RemoteIo::RemoteError>{};
        }

        std::string_view lastSegmentOf(std::string_view path)
        {
            const auto slash = path.find_last_of('/');
            if (slash == std::string_view::npos)
                return path;
            return path.substr(slash + 1u);
        }

        /**
         * @brief The directory that holds the absolute @p path, nothing for the root.
         */
        std::optional<std::string> parentOf(std::string_view path)
        {
            const auto slash = path.find_last_of('/');
            if (slash == std::string_view::npos || path == "/")
                return std::nullopt;
            if (slash == 0u)
                return std::string{"/"};
            return std::string{path.substr(0u, slash)};
        }

        RemoteIo::RemoteError missing()
        {
            return errorOf(RemoteErrorKind::NoSuchFile, "The path does not exist");
        }

        /**
         * @brief The line STAT lists for @p path, when that is one line for @p path and no directory: STAT lists the
         *        content of a directory. vsftpd names the entry by its last segment, proftpd by the whole path.
         *
         * @return Nothing for a directory, a path the server listed nothing for, or lines that tell no single entry.
         *         The replyError() of a refusal.
         */
        asio::awaitable<std::expected<std::optional<ListingEntry>, RemoteIo::RemoteError>>
        statusLineOf(MetadataSession session, std::string path)
        {
            const auto reply =
                co_await session.transfer.control->execute(Command{.verb = Verb::Status, .argument = path});
            if (!reply)
                co_return std::unexpected(reply.error());
            if (!succeeded(*reply))
                co_return std::unexpected(replyError(*reply, Verb::Status));
            // The first and the last line frame what was listed.
            if (reply->lines.size() != 3u)
                co_return std::optional<ListingEntry>{};
            std::string_view line = reply->lines[1];
            // proftpd indents the lines.
            if (line.starts_with(' '))
                line.remove_prefix(1u);
            auto parser = ListingParser::list(today(), session.limits);
            parser.feed(line);
            auto listing = parser.finish();
            const auto name = lastSegmentOf(path);
            if (listing.entries.size() != 1u)
                co_return std::optional<ListingEntry>{};
            auto& entry = listing.entries.front();
            if (entry.type == FileType::Directory || (entry.name != name && entry.name != path))
                co_return std::optional<ListingEntry>{};
            entry.name = std::string{name};
            co_return std::optional<ListingEntry>{std::move(entry)};
        }

        /**
         * @brief lstatPath() from the listing of the parent. Where the parent cannot be listed and the refusal does
         *        not say why, the parent's own entry does.
         */
        asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
        lstatInParent(MetadataSession session, std::string path)
        {
            const auto parent = parentOf(path);
            if (!parent)
            {
                if (auto entered = co_await changeDirectory(session.transfer.control, path); !entered)
                    co_return std::unexpected(std::move(entered).error());
                co_return ListingEntry{.name = path, .type = FileType::Directory};
            }
            auto listing = co_await listDirectory(session, *parent);
            if (listing)
            {
                const auto found = std::ranges::find(listing->entries, lastSegmentOf(path), &ListingEntry::name);
                if (found == listing->entries.end())
                    co_return std::unexpected(missing());
                co_return std::move(*found);
            }
            auto failure = std::move(listing).error();
            if (failure.protocolCode == 0 || failure.kind != RemoteErrorKind::Failure)
                co_return std::unexpected(std::move(failure));

            const auto container = co_await lstatPath(session, *parent);
            if (!container)
                co_return std::unexpected(container.error());
            if (container->type == FileType::Directory)
            {
                failure.kind = RemoteErrorKind::PermissionDenied;
                co_return std::unexpected(std::move(failure));
            }
            // A link that leads nowhere it could be entered, which of both is unknown.
            if (container->type == FileType::Symlink)
                co_return std::unexpected(std::move(failure));
            co_return std::unexpected(missing());
        }

        /**
         * @brief Every name of @p other is in @p listing.
         */
        bool listsAtLeast(Listing const& listing, Listing const& other)
        {
            std::set<std::string_view> names{};
            for (auto const& entry : listing.entries)
                names.insert(entry.name);
            return std::ranges::all_of(
                other.entries,
                [&names](ListingEntry const& entry)
                {
                    return names.contains(entry.name);
                }
            );
        }

        asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>>
        list(MetadataSession session, std::string argument)
        {
            co_return co_await fetchListing(
                session.transfer,
                Command{.verb = Verb::List, .argument = std::move(argument)},
                ListingParser::list(today(), session.limits),
                session.options
            );
        }

        /**
         * @brief LIST -a where the server reads the option, else LIST. The first listing with entries finds out: -a
         *        counts when it lists at least what LIST does, a server that takes it for a path fails or lists less.
         */
        asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>> listWithList(MetadataSession session)
        {
            auto& dialect = *session.dialect;
            if (dialect.listAll() == Support::Works)
                co_return co_await list(session, "-a");
            auto plain = co_await list(session, "");
            if (!plain || dialect.listAll() == Support::Refused || plain->entries.empty())
                co_return plain;

            auto all = co_await list(session, "-a");
            if (all && listsAtLeast(*all, *plain))
            {
                dialect.learnListAll(Support::Works);
                co_return all;
            }
            // Only the server's answer tells, a broken connection or a cancel does not.
            if (all || all.error().protocolCode != 0)
                dialect.learnListAll(Support::Refused);
            if (!all && isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(all.error());
            co_return plain;
        }
    }

    Support ServerDialect::status() const
    {
        return status_.load();
    }

    void ServerDialect::learnStatus(Support support)
    {
        learnOnce(status_, support);
    }

    Support ServerDialect::machineListing() const
    {
        return machineListing_.load();
    }

    void ServerDialect::learnMachineListing(Support support)
    {
        learnOnce(machineListing_, support);
    }

    Support ServerDialect::listAll() const
    {
        return listAll_.load();
    }

    void ServerDialect::learnListAll(Support support)
    {
        learnOnce(listAll_, support);
    }

    asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>>
    listDirectory(MetadataSession session, std::string path)
    {
        if (auto entered = co_await changeDirectory(session.transfer.control, std::move(path)); !entered)
            co_return std::unexpected(std::move(entered).error());
        auto listed = co_await listWithList(session);
        auto& dialect = *session.dialect;
        if (!listed || listed->unreadableLines == 0u || dialect.machineListing() == Support::Refused)
            co_return listed;

        // A LIST format the parser cannot read, MLSD may list what it left out.
        auto machine = co_await fetchListing(
            session.transfer,
            Command{.verb = Verb::MachineListDirectory},
            ListingParser::machineListing(session.limits),
            session.options
        );
        if (machine)
        {
            dialect.learnMachineListing(Support::Works);
            co_return machine;
        }
        if (lacksTheCommand(machine.error()))
            dialect.learnMachineListing(Support::Refused);
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return machine;
        co_return listed;
    }

    asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    lstatPath(MetadataSession session, std::string path)
    {
        auto& dialect = *session.dialect;
        if (dialect.status() != Support::Refused)
        {
            auto line = co_await statusLineOf(session, path);
            if (line)
                dialect.learnStatus(Support::Works);
            else if (lacksTheCommand(line.error()))
                dialect.learnStatus(Support::Refused);
            // Not the server's answer: the connection broke or the call was cancelled.
            else if (line.error().protocolCode == 0)
                co_return std::unexpected(line.error());
            if (line && *line)
                co_return std::move(**line);
        }
        co_return co_await lstatInParent(std::move(session), std::move(path));
    }

    asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    statPath(MetadataSession session, std::string path)
    {
        auto const& control = session.transfer.control;
        const auto name = std::string{lastSegmentOf(path)};
        // Not MLST: proftpd's follows a link for its type but not for its size.
        const auto size = co_await control->execute(Command{.verb = Verb::Size, .argument = path});
        if (!size)
            co_return std::unexpected(size.error());
        if (succeeded(*size))
        {
            const auto bytes = parseSizeReply(*size);
            if (!bytes)
                co_return std::unexpected(bytes.error());
            ListingEntry entry{.name = name, .type = FileType::Regular, .size = *bytes};
            const auto modified = co_await control->execute(Command{.verb = Verb::ModificationTime, .argument = path});
            if (!modified)
                co_return std::unexpected(modified.error());
            if (const auto time = parseModificationTimeReply(*modified))
                entry.modified = *time;
            co_return entry;
        }
        const auto entered = co_await changeDirectory(control, path);
        if (entered)
            co_return ListingEntry{.name = name, .type = FileType::Directory};
        if (entered.error().protocolCode == 0)
            co_return std::unexpected(entered.error());

        auto found = co_await lstatPath(session, path);
        if (found && found->type == FileType::Symlink)
            co_return std::unexpected(
                errorOf(RemoteErrorKind::NoSuchFile, "The target of the link does not exist or cannot be reached")
            );
        co_return found;
    }

    SharedData::DirectoryEntry toDirectoryEntry(ListingEntry const& entry)
    {
        SharedData::DirectoryEntry converted{
            .name = Paths::sanitizedUtf8(entry.name),
            .nameIsValidUtf8 = Paths::isValidUtf8(entry.name),
            .type = entry.type,
            .size = entry.size.value_or(0u),
            .owner = Paths::sanitizedUtf8(entry.owner),
            .group = Paths::sanitizedUtf8(entry.group),
            .permissions = entry.permissions,
        };
        const auto secondsSinceEpoch = [](std::int64_t seconds)
        {
            return static_cast<std::uint64_t>(std::max<std::int64_t>(seconds, 0));
        };
        if (entry.modified)
        {
            const auto seconds = std::chrono::floor<std::chrono::seconds>(*entry.modified);
            converted.mtime = secondsSinceEpoch(seconds.time_since_epoch().count());
            converted.mtimeNsec = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(*entry.modified - seconds).count()
            );
        }
        else if (entry.listedTime)
            converted.mtime = secondsSinceEpoch(entry.listedTime->time.time_since_epoch().count());
        if (entry.linkTarget)
            converted.linkTarget = Paths::sanitizedUtf8(*entry.linkTarget);
        return converted;
    }
}

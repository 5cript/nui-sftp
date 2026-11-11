#include <ftp/client/metadata.hpp>
#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <paths/utf8.hpp>
#include "client_support.hpp"

#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::isCancelled;
        using Detail::succeeded;
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

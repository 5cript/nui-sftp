#include <ftp/client/metadata.hpp>
#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <paths/utf8.hpp>
#include "../protocol/listing_lines.hpp"
#include "client_support.hpp"

#include <boost/asio/this_coro.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

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

        template <typename FactT>
        void learnOnce(std::atomic<FactT>& fact, FactT value)
        {
            auto unknown = FactT::Unknown;
            fact.compare_exchange_strong(unknown, value);
        }

        /**
         * @brief The server answered that it lacks the command: 500, 502 or 504.
         */
        bool lacksTheCommand(RemoteIo::RemoteError const& error)
        {
            return error.kind == RemoteErrorKind::Unsupported;
        }

        /**
         * @brief @p error is a refusal of the server whose text did not tell its kind, as vsftpd's never does. Only
         *        such an error may be followed by more commands: a cancelled coroutine throws at its next co_await.
         */
        bool untold(RemoteIo::RemoteError const& error)
        {
            return error.protocolCode != 0 && error.kind == RemoteErrorKind::Failure;
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
         * @brief The entry a line of STAT lists, which proftpd indents.
         */
        std::optional<ListingEntry> statusEntryOf(std::string_view line)
        {
            if (line.starts_with(' '))
                line.remove_prefix(1u);
            auto read = Detail::readListLine(line, today());
            if (!read)
                return std::nullopt;
            return std::move(read->entry);
        }

        /**
         * @brief STAT lists "." for a directory, so a single line is the path itself and never the one entry of a
         *        directory. Asked once, for the root.
         */
        asio::awaitable<std::expected<bool, RemoteIo::RemoteError>> statusListsDots(MetadataSession session)
        {
            auto& dialect = *session.dialect;
            if (const auto learned = dialect.statusListsDots(); learned != Support::Unknown)
                co_return learned == Support::Works;
            const auto reply =
                co_await session.transfer.control->execute(Command{.verb = Verb::Status, .argument = "/"});
            if (!reply)
                co_return std::unexpected(reply.error());
            const auto listsDots = succeeded(*reply) &&
                std::ranges::any_of(
                    reply->lines,
                    [](std::string const& line)
                    {
                        const auto entry = statusEntryOf(line);
                        return entry && entry->name == ".";
                    }
                );
            dialect.learnStatusListsDots(listsDots ? Support::Works : Support::Refused);
            co_return listsDots;
        }

        /**
         * @brief The line STAT lists for @p path, when that is one line for @p path and no directory: STAT lists the
         *        content of a directory. proftpd names the entry by the whole path, which no entry of a directory
         *        has. vsftpd names it by its last segment, like the one entry of a directory of that name: such a line
         *        counts where statusListsDots().
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
            auto entry = statusEntryOf(reply->lines[1]);
            if (!entry || entry->type == FileType::Directory)
                co_return std::optional<ListingEntry>{};
            const auto name = lastSegmentOf(path);
            if (entry->name != path)
            {
                if (entry->name != name)
                    co_return std::optional<ListingEntry>{};
                const auto listsDots = co_await statusListsDots(session);
                if (!listsDots)
                    co_return std::unexpected(listsDots.error());
                if (!*listsDots)
                    co_return std::optional<ListingEntry>{};
            }
            entry->name = std::string{name};
            co_return entry;
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
            if (!untold(failure))
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
         * @brief Sends @p command and waits for its final reply.
         *
         * @return The reply when it is positive, else its replyError().
         */
        asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
        perform(std::shared_ptr<ControlConnection> control, Command command)
        {
            const auto verb = command.verb;
            auto reply = co_await control->execute(std::move(command));
            if (!reply)
                co_return std::unexpected(std::move(reply).error());
            if (!succeeded(*reply))
                co_return std::unexpected(replyError(*reply, verb));
            co_return std::move(*reply);
        }

        /** @brief The kind of a refusal on a path that exists, given its entry. */
        using KindOfExisting = std::function<RemoteErrorKind(ListingEntry const&)>;

        RemoteErrorKind denied(ListingEntry const&)
        {
            return RemoteErrorKind::PermissionDenied;
        }

        /**
         * @brief A refusal with its kind, and the entry at its path where the lookup found one.
         */
        struct Explanation
        {
            RemoteIo::RemoteError error{};
            std::optional<ListingEntry> entry{};
        };

        /**
         * @brief Gives the untold() refusal @p error of a command on @p path its kind: NoSuchFile where @p path does
         *        not exist, else what @p kindOf says for its entry, which the explanation keeps.
         */
        asio::awaitable<Explanation>
        explained(MetadataSession session, std::string path, RemoteIo::RemoteError error, KindOfExisting kindOf)
        {
            assert(untold(error) && "only a refusal that tells no kind is explained");
            auto found = co_await lstatPath(std::move(session), std::move(path));
            if (found)
            {
                error.kind = kindOf(*found);
                co_return Explanation{.error = std::move(error), .entry = std::move(*found)};
            }
            if (found.error().kind == RemoteErrorKind::NoSuchFile)
                error.kind = RemoteErrorKind::NoSuchFile;
            co_return Explanation{.error = std::move(error)};
        }

        /**
         * @brief explained() for a caller that needs the refusal alone.
         */
        asio::awaitable<RemoteIo::RemoteError>
        explain(MetadataSession session, std::string path, RemoteIo::RemoteError error, KindOfExisting kindOf)
        {
            co_return (co_await explained(std::move(session), std::move(path), std::move(error), std::move(kindOf)))
                .error;
        }

        /**
         * @brief Nothing can be made at @p path: its parent does not exist or is no directory.
         */
        asio::awaitable<bool> lacksItsDirectory(MetadataSession session, std::string_view path)
        {
            const auto parent = parentOf(path);
            if (!parent)
                co_return false;
            const auto container = co_await statPath(std::move(session), *parent);
            if (container)
                co_return container->type != FileType::Directory;
            co_return container.error().kind == RemoteErrorKind::NoSuchFile;
        }

        /**
         * @brief Gives the untold() refusal @p error of a command that makes @p path its kind: AlreadyExists where
         *        something is at @p path, NoSuchFile where its parent is no directory. That @p path does not exist
         *        tells nothing here, a refusal in a directory that exists stays as the server gave it.
         */
        asio::awaitable<RemoteIo::RemoteError>
        explainNew(MetadataSession session, std::string path, RemoteIo::RemoteError error)
        {
            assert(untold(error) && "only a refusal that tells no kind is explained");
            const auto found = co_await lstatPath(session, path);
            if (found)
            {
                error.kind = RemoteErrorKind::AlreadyExists;
                co_return error;
            }
            if (found.error().kind != RemoteErrorKind::NoSuchFile)
                co_return error;
            if (co_await lacksItsDirectory(std::move(session), path))
                error.kind = RemoteErrorKind::NoSuchFile;
            co_return error;
        }

        Command setterCommand(ModificationTimeSetter setter, std::string_view time, std::string_view path)
        {
            switch (setter)
            {
                case ModificationTimeSetter::ModifyFact:
                    return Command{.verb = Verb::SetModificationTime, .argument = fmt::format("{} {}", time, path)};
                case ModificationTimeSetter::ModificationTimeWithTime:
                    return Command{.verb = Verb::ModificationTime, .argument = fmt::format("{} {}", time, path)};
                case ModificationTimeSetter::SiteUtime:
                    return Command{.verb = Verb::Site, .argument = fmt::format("UTIME {} {}", time, path)};
                case ModificationTimeSetter::Unknown:
                case ModificationTimeSetter::None:
                    break;
            }
            assert(false && "only a command sets a modification time");
            return Command{.verb = Verb::Noop};
        }

        RemoteIo::RemoteError noSetter()
        {
            return errorOf(RemoteErrorKind::Unsupported, "The server sets no modification times");
        }

        /**
         * @brief The error of a reply to a setter: a 202 is a command not implemented (RFC 959 4.2), the time stays as
         *        it was; a 501 is a time the server does not take (MFMT draft 3.2).
         */
        RemoteIo::RemoteError setterError(Reply const& reply, Verb verb)
        {
            if (reply.code != 202 && reply.code != 501)
                return replyError(reply, verb);
            return RemoteIo::RemoteError{
                .kind = RemoteErrorKind::Unsupported,
                .protocol = RemoteIo::Protocol::Ftp,
                .protocolCode = reply.code,
                .message = reply.text(),
            };
        }

        /**
         * @brief How many names of @p other are in @p listing.
         */
        std::size_t sharedNames(Listing const& listing, Listing const& other)
        {
            std::set<std::string_view> names{};
            for (auto const& entry : listing.entries)
                names.insert(entry.name);
            return static_cast<std::size_t>(std::ranges::count_if(
                other.entries,
                [&names](ListingEntry const& entry)
                {
                    return names.contains(entry.name);
                }
            ));
        }

        /**
         * @brief The refusal @p error is about the connections of a transfer, not about what was asked for.
         */
        bool refusedTheTransfer(RemoteIo::RemoteError const& error)
        {
            switch (error.kind)
            {
                case RemoteErrorKind::NoConnection:
                case RemoteErrorKind::ConnectionLost:
                case RemoteErrorKind::ConnectionLimit:
                case RemoteErrorKind::NoSecureConnection:
                case RemoteErrorKind::TimedOut:
                case RemoteErrorKind::Busy:
                    return true;
                default:
                    return false;
            }
        }

        /**
         * @brief Adds the entries of @p machine that @p listed lacks, up to @p limits. The entries LIST read stay:
         *        only they name the target of a link. What is left unreadable is what neither listing read.
         */
        void addMissingEntries(Listing& listed, Listing machine, ListingLimits const& limits)
        {
            std::vector<ListingEntry> missing{};
            {
                std::set<std::string_view> names{};
                for (auto const& entry : listed.entries)
                    names.insert(entry.name);
                for (auto& entry : machine.entries)
                {
                    if (!names.contains(entry.name))
                        missing.push_back(std::move(entry));
                }
            }
            for (auto& entry : missing)
            {
                if (listed.entries.size() >= limits.entryCount)
                    ++listed.droppedEntries;
                else
                    listed.entries.push_back(std::move(entry));
            }
            listed.unreadableLines = std::min(listed.unreadableLines, machine.unreadableLines);
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
         * @brief LIST -a where the server reads the option, else LIST. Both are asked until an answer tells: -a counts
         *        when it lists entries and at least what LIST does, so also hidden entries in a directory LIST shows
         *        empty. A server that takes it for a path refuses it or lists none of them. Some of them are a
         *        directory that changed in between, which tells nothing, like two empty listings.
         */
        asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>> listWithList(MetadataSession session)
        {
            auto& dialect = *session.dialect;
            if (dialect.listAll() == Support::Works)
                co_return co_await list(session, "-a");
            auto plain = co_await list(session, "");
            if (!plain || dialect.listAll() == Support::Refused)
                co_return plain;
            // Lines in no format the parser reads show no empty directory.
            if (plain->entries.empty() && plain->unreadableLines != 0u)
                co_return plain;

            auto all = co_await list(session, "-a");
            if (!all)
            {
                if (isCancelled(co_await asio::this_coro::cancellation_state))
                    co_return std::unexpected(all.error());
                // Only a refusal of what was asked for tells, a broken or refused connection does not.
                if (all.error().protocolCode != 0 && !refusedTheTransfer(all.error()))
                    dialect.learnListAll(Support::Refused);
                co_return plain;
            }
            const auto shared = sharedNames(*all, *plain);
            if (shared == plain->entries.size() && !all->entries.empty())
            {
                dialect.learnListAll(Support::Works);
                co_return all;
            }
            if (shared == 0u && !plain->entries.empty())
                dialect.learnListAll(Support::Refused);
            co_return plain;
        }
    }

    Support ServerDialect::statusListsDots() const
    {
        return statusListsDots_.load();
    }

    void ServerDialect::learnStatusListsDots(Support support)
    {
        learnOnce(statusListsDots_, support);
    }

    Support ServerDialect::status() const
    {
        return status_.load();
    }

    void ServerDialect::learnStatus(Support support)
    {
        learnOnce(status_, support);
    }

    ModificationTimeSetter ServerDialect::modificationTimeSetter() const
    {
        return modificationTimeSetter_.load();
    }

    void ServerDialect::learnModificationTimeSetter(ModificationTimeSetter setter)
    {
        learnOnce(modificationTimeSetter_, setter);
    }

    Support ServerDialect::machineListSingle() const
    {
        return machineListSingle_.load();
    }

    void ServerDialect::learnMachineListSingle(Support support)
    {
        if (support == Support::Refused)
            machineListSingle_.store(support);
        else
            learnOnce(machineListSingle_, support);
    }

    Support ServerDialect::size() const
    {
        return size_.load();
    }

    void ServerDialect::learnSize(Support support)
    {
        if (support == Support::Refused)
            size_.store(support);
        else
            learnOnce(size_, support);
    }

    void ServerDialect::learnFrom(Features const& features)
    {
        if (features.has("MLST"))
            learnMachineListSingle(Support::Works);
        if (features.has("SIZE"))
            learnSize(Support::Works);
    }

    bool ServerDialect::probesEntries() const
    {
        return machineListSingle() == Support::Works || size() == Support::Works;
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

        // A LIST format the parser cannot read, MLSD may list what it left out. Not more: it leaves out links.
        auto machine = co_await fetchListing(
            session.transfer,
            Command{.verb = Verb::MachineListDirectory},
            ListingParser::machineListing(session.limits),
            session.options
        );
        if (machine)
        {
            dialect.learnMachineListing(Support::Works);
            addMissingEntries(*listed, std::move(*machine), session.limits);
            co_return listed;
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

    namespace
    {
        RemoteIo::RemoteError unreachableTarget()
        {
            return errorOf(RemoteErrorKind::NoSuchFile, "The target of the link does not exist or cannot be reached");
        }

        /**
         * @brief A refusal that tells nothing of the path it answers: the server lacks the command, or it failed for
         *        now (4yz) without naming a missing path.
         */
        bool tellsNothingOfThePath(RemoteIo::RemoteError const& refusal)
        {
            return lacksTheCommand(refusal) ||
                (classifyReply(refusal.protocolCode) == ReplyClass::TransientNegative &&
                    refusal.kind != RemoteErrorKind::NoSuchFile);
        }

        /**
         * @brief What followedEntry() makes of a refusal that tellsNothingOfThePath().
         */
        enum class UntellingRefusal
        {
            /** @brief No answer, something else looks the path up after it. */
            IsNoAnswer,
            /** @brief The error of the lookup, nothing else tells whether the path exists. */
            Fails,
        };

        /**
         * @brief The file of @p bytes that SIZE told of @p path, with its time where MDTM tells one.
         */
        asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
        timedFile(std::shared_ptr<ControlConnection> control, std::string path, std::uint64_t bytes)
        {
            ListingEntry entry{.name = std::string{lastSegmentOf(path)}, .type = FileType::Regular, .size = bytes};
            const auto modified =
                co_await control->execute(Command{.verb = Verb::ModificationTime, .argument = std::move(path)});
            if (!modified)
                co_return std::unexpected(modified.error());
            if (const auto time = parseModificationTimeReply(*modified))
                entry.modified = *time;
            co_return entry;
        }

        /**
         * @brief What SIZE and MDTM, then CWD tell of @p path, which all follow links.
         *
         * @return The entry, named like the last segment of @p path, nothing when none of them answers, or SIZE does
         * not and @p lookingFor is a file. The replyError() of a refusal that tellsNothingOfThePath() where
         *         @p untelling says so.
         */
        asio::awaitable<std::expected<std::optional<ListingEntry>, RemoteIo::RemoteError>> followedEntry(
            MetadataSession session,
            std::string path,
            UntellingRefusal untelling,
            RemoteIo::ProbeFor lookingFor = RemoteIo::ProbeFor::AnyEntry
        )
        {
            auto const& control = session.transfer.control;
            const auto fails = [untelling](RemoteIo::RemoteError const& refusal)
            {
                return untelling == UntellingRefusal::Fails && tellsNothingOfThePath(refusal);
            };
            // Not MLST: proftpd's follows a link for its type but not for its size.
            const auto size = co_await control->execute(Command{.verb = Verb::Size, .argument = path});
            if (!size)
                co_return std::unexpected(size.error());
            if (succeeded(*size))
            {
                const auto bytes = parseSizeReply(*size);
                if (!bytes)
                    co_return std::unexpected(bytes.error());
                co_return co_await timedFile(control, std::move(path), *bytes);
            }
            if (auto refusal = replyError(*size, Verb::Size); fails(refusal))
                co_return std::unexpected(std::move(refusal));
            if (lookingFor == RemoteIo::ProbeFor::File)
                co_return std::nullopt;
            const auto entered = co_await changeDirectory(control, path);
            if (entered)
                co_return ListingEntry{.name = std::string{lastSegmentOf(path)}, .type = FileType::Directory};
            if (entered.error().protocolCode == 0 || fails(entered.error()))
                co_return std::unexpected(entered.error());
            co_return std::nullopt;
        }
    }

    asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    statPath(MetadataSession session, std::string path)
    {
        auto followed = co_await followedEntry(session, path, UntellingRefusal::IsNoAnswer);
        if (!followed)
            co_return std::unexpected(std::move(followed).error());
        if (*followed)
            co_return std::move(**followed);

        auto found = co_await lstatPath(session, path);
        if (found && found->type == FileType::Symlink)
            co_return std::unexpected(unreachableTarget());
        co_return found;
    }

    asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    statLinkTarget(MetadataSession session, std::string path)
    {
        auto followed = co_await followedEntry(std::move(session), std::move(path), UntellingRefusal::IsNoAnswer);
        if (!followed)
            co_return std::unexpected(std::move(followed).error());
        if (*followed)
            co_return std::move(**followed);
        co_return std::unexpected(unreachableTarget());
    }

    namespace
    {
        RemoteIo::RemoteError cannotProbe()
        {
            return errorOf(RemoteErrorKind::Unsupported, "The server has no command to look up a single path");
        }

        /**
         * @brief What MLST tells of @p path.
         *
         * @return The entry, named like the last segment of @p path, nothing for a refusal that tells no more than that
         *         the path cannot be listed. Unsupported where the server lacks MLST, the replyError() of a refusal
         *         for now (4yz).
         */
        asio::awaitable<std::expected<std::optional<ListingEntry>, RemoteIo::RemoteError>>
        machineListedEntry(MetadataSession session, std::string path)
        {
            const auto reply =
                co_await session.transfer.control->execute(Command{.verb = Verb::MachineListSingle, .argument = path});
            if (!reply)
                co_return std::unexpected(reply.error());
            if (!succeeded(*reply))
            {
                auto refusal = replyError(*reply, Verb::MachineListSingle);
                if (refusal.kind == RemoteErrorKind::NoSuchFile || (untold(refusal) && !tellsNothingOfThePath(refusal)))
                    co_return std::optional<ListingEntry>{};
                co_return std::unexpected(std::move(refusal));
            }
            auto entry = parseMachineListReply(*reply);
            if (!entry)
                co_return std::unexpected(std::move(entry).error());
            entry->name = std::string{lastSegmentOf(path)};
            co_return std::optional<ListingEntry>{std::move(*entry)};
        }

        /**
         * @brief The entry of a lookup, NoSuchFile when it found nothing.
         */
        std::expected<ListingEntry, RemoteIo::RemoteError>
        foundOrMissing(std::expected<std::optional<ListingEntry>, RemoteIo::RemoteError> found)
        {
            if (!found)
                return std::unexpected(std::move(found).error());
            if (!*found)
                return std::unexpected(missing());
            return std::move(**found);
        }
    }

    asio::awaitable<std::expected<ListingEntry, RemoteIo::RemoteError>>
    probePath(MetadataSession session, std::string path, RemoteIo::ProbeFor lookingFor)
    {
        auto& dialect = *session.dialect;
        if (dialect.machineListSingle() == Support::Works)
        {
            auto listed = co_await machineListedEntry(session, path);
            if (listed || !lacksTheCommand(listed.error()))
                co_return foundOrMissing(std::move(listed));
            dialect.learnMachineListSingle(Support::Refused);
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(std::move(listed).error());
        }
        if (dialect.size() != Support::Works)
            co_return std::unexpected(cannotProbe());
        auto sized = co_await followedEntry(std::move(session), std::move(path), UntellingRefusal::Fails, lookingFor);
        if (sized || !lacksTheCommand(sized.error()))
            co_return foundOrMissing(std::move(sized));
        dialect.learnSize(Support::Refused);
        co_return std::unexpected(cannotProbe());
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> makeDirectory(MetadataSession session, std::string path)
    {
        const auto made =
            co_await perform(session.transfer.control, Command{.verb = Verb::MakeDirectory, .argument = path});
        if (made)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        if (!untold(made.error()))
            co_return std::unexpected(made.error());
        co_return std::unexpected(co_await explainNew(std::move(session), std::move(path), made.error()));
    }

    namespace
    {
        /**
         * @brief removeFile() with the entry that explained a refusal of the DELE.
         */
        asio::awaitable<std::expected<void, Explanation>> deletedFile(MetadataSession session, std::string path)
        {
            const auto removed =
                co_await perform(session.transfer.control, Command{.verb = Verb::Delete, .argument = path});
            if (removed)
                co_return std::expected<void, Explanation>{};
            if (!untold(removed.error()))
                co_return std::unexpected(Explanation{.error = removed.error()});
            co_return std::unexpected(
                co_await explained(
                    std::move(session),
                    std::move(path),
                    removed.error(),
                    [](ListingEntry const& entry)
                    {
                        if (entry.type == FileType::Directory)
                            return RemoteErrorKind::Failure;
                        return RemoteErrorKind::PermissionDenied;
                    }
                )
            );
        }
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> removeFile(MetadataSession session, std::string path)
    {
        co_return (co_await deletedFile(std::move(session), std::move(path)))
            .transform_error(
                [](Explanation refusal)
                {
                    return std::move(refusal.error);
                }
            );
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> removeEntry(MetadataSession session, std::string path)
    {
        auto removed = co_await deletedFile(session, path);
        if (removed)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        auto& refusal = removed.error();
        if (refusal.entry && refusal.entry->type == FileType::Directory)
            co_return co_await removeDirectory(std::move(session), std::move(path));
        co_return std::unexpected(std::move(refusal.error));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    removeDirectory(MetadataSession session, std::string path)
    {
        // A listing left the working directory in the path, which a server may refuse to remove.
        if (const auto parent = parentOf(path))
        {
            const auto left = co_await changeDirectory(session.transfer.control, *parent);
            if (!left && left.error().protocolCode == 0)
                co_return std::unexpected(left.error());
        }
        const auto removed =
            co_await perform(session.transfer.control, Command{.verb = Verb::RemoveEmptyDirectory, .argument = path});
        if (removed)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        auto error = removed.error();
        if (!untold(error))
            co_return std::unexpected(std::move(error));

        // vsftpd refuses a missing path, a file, a directory with content and a denied one alike. Not STAT, which
        // lists all the content of a directory.
        const auto found = co_await lstatInParent(session, path);
        if (!found)
        {
            if (found.error().kind == RemoteErrorKind::NoSuchFile)
                error.kind = RemoteErrorKind::NoSuchFile;
            co_return std::unexpected(std::move(error));
        }
        if (found->type != FileType::Directory)
            co_return std::unexpected(std::move(error));
        const auto content = co_await listDirectory(std::move(session), std::move(path));
        if (content && !content->entries.empty())
        {
            error.message = "The directory is not empty";
            co_return std::unexpected(std::move(error));
        }
        error.kind = RemoteErrorKind::PermissionDenied;
        co_return std::unexpected(std::move(error));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    renamePath(MetadataSession session, std::string from, std::string to)
    {
        auto const& control = session.transfer.control;
        const auto source = co_await control->execute(Command{.verb = Verb::RenameFrom, .argument = from});
        if (!source)
            co_return std::unexpected(source.error());
        // RNFR goes on with 350, anything else ends the rename (RFC 959 5.4).
        if (source->code != 350)
        {
            auto refusal = replyError(*source, Verb::RenameFrom);
            if (!untold(refusal))
                co_return std::unexpected(std::move(refusal));
            co_return std::unexpected(co_await explain(session, std::move(from), std::move(refusal), denied));
        }
        const auto renamed = co_await perform(control, Command{.verb = Verb::RenameTo, .argument = to});
        if (renamed)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        if (!untold(renamed.error()))
            co_return std::unexpected(renamed.error());
        co_return std::unexpected(co_await explainNew(std::move(session), std::move(to), renamed.error()));
    }

    asio::awaitable<std::expected<std::uint64_t, RemoteIo::RemoteError>>
    sizeOf(MetadataSession session, std::string path)
    {
        const auto reply = co_await perform(session.transfer.control, Command{.verb = Verb::Size, .argument = path});
        if (reply)
            co_return parseSizeReply(*reply);
        if (!untold(reply.error()))
            co_return std::unexpected(reply.error());
        co_return std::unexpected(
            co_await explain(
                std::move(session),
                std::move(path),
                reply.error(),
                [](ListingEntry const& entry)
                {
                    if (entry.type == FileType::Regular)
                        return RemoteErrorKind::PermissionDenied;
                    return RemoteErrorKind::Failure;
                }
            )
        );
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    setModificationTime(MetadataSession session, std::string path, std::chrono::sys_seconds time)
    {
        const auto timeVal = formatTimeVal(time);
        if (!timeVal)
            co_return std::unexpected(
                errorOf(RemoteErrorKind::Unsupported, "FTP carries no time before year 1000 or after 9999")
            );
        auto& dialect = *session.dialect;
        const auto learned = dialect.modificationTimeSetter();
        if (learned == ModificationTimeSetter::None)
            co_return std::unexpected(noSetter());

        constexpr std::array<ModificationTimeSetter, 3> setters{
            ModificationTimeSetter::ModifyFact,
            ModificationTimeSetter::ModificationTimeWithTime,
            ModificationTimeSetter::SiteUtime,
        };
        // A 550 to MDTM with a time for a file that exists, which may have refused to set the time.
        std::optional<RemoteIo::RemoteError> undecided{};
        for (const auto setter : setters)
        {
            if (learned != ModificationTimeSetter::Unknown && setter != learned)
                continue;
            const auto command = setterCommand(setter, *timeVal, path);
            const auto reply = co_await session.transfer.control->execute(command);
            if (!reply)
                co_return std::unexpected(reply.error());
            if (succeeded(*reply) && reply->code != 202)
            {
                dialect.learnModificationTimeSetter(setter);
                co_return std::expected<void, RemoteIo::RemoteError>{};
            }
            auto error = setterError(*reply, command.verb);
            const auto probing = learned == ModificationTimeSetter::Unknown;
            // 501: arguments in a form the server does not know.
            if (probing && (lacksTheCommand(error) || reply->code == 501))
                continue;
            if (probing && setter == ModificationTimeSetter::ModificationTimeWithTime && reply->code == 550)
            {
                const auto found = co_await lstatPath(session, path);
                if (!found)
                    co_return std::unexpected(found.error());
                // A server without this form reads the time as part of a file name and finds none. Only a text that
                // says so tells that from a server that knows the form and refused.
                if (error.kind != RemoteErrorKind::NoSuchFile)
                    undecided = std::move(error);
                continue;
            }
            // A 4yz is temporary (MFMT draft 3.2), the path explains no such refusal.
            if (!untold(error) || classifyReply(reply->code) == ReplyClass::TransientNegative)
                co_return std::unexpected(std::move(error));
            co_return std::unexpected(co_await explain(std::move(session), std::move(path), std::move(error), denied));
        }
        if (undecided)
        {
            // The other forms are missing, so this is the one the server may take.
            dialect.learnModificationTimeSetter(ModificationTimeSetter::ModificationTimeWithTime);
            if (undecided->kind == RemoteErrorKind::Failure)
                undecided->kind = RemoteErrorKind::PermissionDenied;
            co_return std::unexpected(std::move(*undecided));
        }
        dialect.learnModificationTimeSetter(ModificationTimeSetter::None);
        co_return std::unexpected(noSetter());
    }

    namespace
    {
        /**
         * @brief canonicalDirectory() with the entry that explained a refusal of the CWD.
         */
        asio::awaitable<std::expected<std::string, Explanation>>
        enteredDirectory(MetadataSession session, std::string path)
        {
            const auto control = session.transfer.control;
            const auto entered = co_await changeDirectory(control, path);
            if (!entered && !untold(entered.error()))
                co_return std::unexpected(Explanation{.error = entered.error()});
            if (!entered)
                co_return std::unexpected(
                    co_await explained(
                        std::move(session),
                        std::move(path),
                        entered.error(),
                        [](ListingEntry const& entry)
                        {
                            if (entry.type == FileType::Directory)
                                return RemoteErrorKind::PermissionDenied;
                            return RemoteErrorKind::Failure;
                        }
                    )
                );
            const auto reply = co_await perform(control, Command{.verb = Verb::PrintWorkingDirectory});
            if (!reply)
                co_return std::unexpected(Explanation{.error = reply.error()});
            co_return parsePathReply(*reply, Verb::PrintWorkingDirectory)
                .transform_error(
                    [](RemoteIo::RemoteError error)
                    {
                        return Explanation{.error = std::move(error)};
                    }
                );
        }
    }

    asio::awaitable<std::expected<std::string, RemoteIo::RemoteError>>
    canonicalDirectory(MetadataSession session, std::string path)
    {
        co_return (co_await enteredDirectory(std::move(session), std::move(path)))
            .transform_error(
                [](Explanation refusal)
                {
                    return std::move(refusal.error);
                }
            );
    }

    asio::awaitable<std::expected<DirectoryOrEntry, RemoteIo::RemoteError>>
    canonicalDirectoryOrEntry(MetadataSession session, std::string path)
    {
        auto entered = co_await enteredDirectory(std::move(session), std::move(path));
        if (entered)
            co_return DirectoryOrEntry{std::move(*entered)};
        auto& refusal = entered.error();
        if (refusal.entry && refusal.entry->type != FileType::Directory)
            co_return DirectoryOrEntry{std::move(*refusal.entry)};
        co_return std::unexpected(std::move(refusal.error));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    createEmptyFile(MetadataSession session, std::string path)
    {
        const auto found = co_await lstatPath(session, path);
        if (found)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        if (found.error().kind != RemoteErrorKind::NoSuchFile)
            co_return std::unexpected(found.error());

        auto upload = co_await Upload::start(session.transfer, UploadRequest{.path = path}, session.options);
        if (!upload)
        {
            auto error = upload.error();
            if (error.protocolCode == 0 || error.kind == RemoteErrorKind::NoSuchFile)
                co_return std::unexpected(std::move(error));
            // vsftpd refuses a STOR below a missing directory like a denied one, with 553.
            if (co_await lacksItsDirectory(std::move(session), path))
                error.kind = RemoteErrorKind::NoSuchFile;
            co_return std::unexpected(std::move(error));
        }
        const auto finished = co_await (*upload)->finish();
        if (!finished)
            co_return std::unexpected(finished.error());
        co_return std::expected<void, RemoteIo::RemoteError>{};
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
        {
            converted.mtime = secondsSinceEpoch(entry.listedTime->time.time_since_epoch().count());
            converted.mtimePrecision = entry.listedTime->precision == ListedTimePrecision::Day
                ? SharedData::TimePrecision::Day
                : SharedData::TimePrecision::Minute;
        }
        if (entry.linkTarget)
            converted.linkTarget = Paths::sanitizedUtf8(*entry.linkTarget);
        return converted;
    }
}

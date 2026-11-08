#include <ftp/protocol/listing.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_error.hpp>
#include "listing_lines.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace FileTransferProtocol
{
    namespace
    {
        /**
         * @brief @p name can name a child of the listed directory, see Listing::refusedNames.
         */
        bool namesChild(std::string_view name)
        {
            return !name.empty() && name.size() <= ListingParser::maximumNameLength &&
                name.find_first_of(std::string_view{"/\0\r", 3u}) == std::string_view::npos;
        }

        /**
         * @brief For each entry, the entry kept in its place: itself, nothing for a repeated name, and for the first
         *        of a name the first directory among the entries of that name, or itself.
         */
        std::vector<std::optional<std::size_t>> keptInPlaceOf(std::vector<ListingEntry> const& entries)
        {
            std::unordered_map<std::string_view, std::size_t> firstOfName{};
            std::vector<std::optional<std::size_t>> kept(entries.size());
            for (std::size_t index = 0u; index < entries.size(); ++index)
            {
                const auto [first, isFirst] = firstOfName.try_emplace(entries[index].name, index);
                if (isFirst)
                {
                    kept[index] = index;
                    continue;
                }
                auto& keptOfName = kept[first->second];
                if (entries[*keptOfName].type != SharedData::FileType::Directory &&
                    entries[index].type == SharedData::FileType::Directory)
                    keptOfName = index;
            }
            return kept;
        }

        /**
         * @brief Keeps one entry of each name, in the place of the first, see keptInPlaceOf.
         *
         * @return How many were left out.
         */
        std::size_t dropRepeatedNames(std::vector<ListingEntry>& entries)
        {
            const auto keptInPlace = keptInPlaceOf(entries);
            const auto count = static_cast<std::size_t>(std::ranges::count(keptInPlace, std::optional<std::size_t>{}));
            if (count == 0u)
                return 0u;

            std::vector<ListingEntry> kept{};
            kept.reserve(entries.size() - count);
            for (auto const& index : keptInPlace)
            {
                if (index)
                    kept.push_back(std::move(entries[*index]));
            }
            entries = std::move(kept);
            return count;
        }
    }

    ListingParser::ListingParser(
        Format format,
        ListingLimits limits,
        std::chrono::sys_days today,
        std::string listedPrefix
    )
        : format_{format}
        , limits_{limits}
        , today_{today}
        , listedPrefix_{std::move(listedPrefix)}
    {}

    ListingParser ListingParser::machineListing(ListingLimits limits)
    {
        return ListingParser{Format::Machine, limits, {}, {}};
    }

    ListingParser ListingParser::list(std::chrono::sys_days today, ListingLimits limits)
    {
        return ListingParser{Format::List, limits, today, {}};
    }

    ListingParser ListingParser::nameList(std::string_view listedPath, ListingLimits limits)
    {
        std::string prefix{listedPath};
        if (!prefix.empty() && prefix.back() != '/')
            prefix.push_back('/');
        return ListingParser{Format::NameList, limits, {}, std::move(prefix)};
    }

    void ListingParser::feed(std::string_view bytes)
    {
        std::size_t position = 0u;
        while (position < bytes.size())
        {
            const auto lineEnd = bytes.find('\n', position);
            const auto lineEnds = lineEnd != std::string_view::npos;
            const auto piece = bytes.substr(position, lineEnds ? lineEnd - position : std::string_view::npos);
            position = lineEnds ? lineEnd + 1u : bytes.size();

            const auto length = pending_.size() + piece.size() + (lineEnds ? 1u : 0u);
            if (!skippingLongLine_ && length > limits_.lineLength)
            {
                ++listing_.unreadableLines;
                pending_.clear();
                skippingLongLine_ = true;
            }
            if (!skippingLongLine_ && lineEnds && pending_.empty())
                readLine(piece);
            else if (!skippingLongLine_)
            {
                pending_.append(piece);
                if (lineEnds)
                    readLine(pending_);
            }
            if (lineEnds)
            {
                pending_.clear();
                skippingLongLine_ = false;
            }
        }
    }

    Listing ListingParser::finish()
    {
        if (!skippingLongLine_ && !pending_.empty())
            readLine(pending_);
        pending_.clear();
        skippingLongLine_ = false;

        auto listing = std::exchange(listing_, Listing{});
        listing.repeatedNames = dropRepeatedNames(listing.entries);
        return listing;
    }

    void ListingParser::readLine(std::string_view line)
    {
        if (line.ends_with('\r'))
            line.remove_suffix(1u);
        if (line.empty())
            return;

        if (format_ == Format::NameList)
        {
            if (!listedPrefix_.empty() && line.starts_with(listedPrefix_))
                line.remove_prefix(listedPrefix_.size());
            keep(ListingEntry{.name = std::string{line}});
            return;
        }

        auto read = format_ == Format::Machine ? Detail::readMachineListingLine(line, Detail::CurrentAndParent::Skip)
                                               : Detail::readListLine(line, today_);
        if (!read)
        {
            ++listing_.unreadableLines;
            return;
        }
        listing_.unreadableFacts += read->unreadableFacts;
        if (read->entry)
            keep(std::move(*read->entry));
    }

    void ListingParser::keep(ListingEntry entry)
    {
        if (entry.name == "." || entry.name == "..")
            return;
        if (!namesChild(entry.name))
        {
            ++listing_.refusedNames;
            return;
        }
        if (listing_.entries.size() >= limits_.entryCount)
        {
            ++listing_.droppedEntries;
            return;
        }
        listing_.entries.push_back(std::move(entry));
    }

    std::expected<ListingEntry, RemoteIo::RemoteError> parseMachineListReply(Reply const& reply)
    {
        if (reply.code != 250)
            return std::unexpected(replyError(reply, Verb::MachineListSingle));
        const auto entryLine = std::ranges::find_if(
            reply.lines,
            [](std::string const& line)
            {
                return line.starts_with(' ');
            }
        );
        if (entryLine == reply.lines.end())
            return std::unexpected(unreadableReply("The reply to MLST holds no entry", reply.code));

        auto read = Detail::readMachineListingLine(
            std::string_view{*entryLine}.substr(1u), Detail::CurrentAndParent::AsDirectory
        );
        if (!read || !read->entry)
            return std::unexpected(unreadableReply("The entry of the reply to MLST cannot be read", reply.code));
        return std::move(*read->entry);
    }
}

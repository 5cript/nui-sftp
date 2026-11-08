#pragma once

#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <remote_io/remote_error.hpp>
#include <shared_data/directory_entry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol
{
    /**
     * @brief How exact a time LIST printed is.
     */
    enum class ListedTimePrecision : std::uint8_t
    {
        /** @brief A time of day without seconds, for entries younger than about six months. */
        Minute,
        /** @brief A year instead of a time of day. */
        Day,
    };

    /**
     * @brief A modification time as LIST prints it: the clock of the server in a time zone the listing does not
     *        name. vsftpd and proftpd print UTC, pure-ftpd and IIS their local time.
     */
    struct ListedTime
    {
        std::chrono::local_seconds time{};
        ListedTimePrecision precision{ListedTimePrecision::Minute};

        bool operator==(ListedTime const&) const = default;
    };

    /**
     * @brief One entry of a directory listing, with what its format told.
     */
    struct ListingEntry
    {
        /**
         * @brief The name as the server sent it. Bytes, UTF-8 only when the server uses it, so a name that is no
         *        UTF-8 still names the item in later commands.
         */
        std::string name{};
        SharedData::FileType type{SharedData::FileType::Unknown};
        /** @brief The size of a file, approximate for MLSx (RFC 3659 7.5.7). Nothing for devices. */
        std::optional<std::uint64_t> size{};
        /** @brief The modify fact of MLSx, UTC. */
        std::optional<TimeVal> modified{};
        /** @brief The time LIST printed. */
        std::optional<ListedTime> listedTime{};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        /** @brief The owner as listed, a name or a numeric id. Empty when not listed. */
        std::string owner{};
        /** @brief The group as listed, a name or a numeric id. Empty when not listed. */
        std::string group{};
        /** @brief What a symlink points to, when the listing names it. */
        std::optional<std::string> linkTarget{};
        /**
         * @brief The unique fact of MLSx, opaque and case sensitive (RFC 3659 7.5.2). Names with equal values are
         *        one item, which proftpd also reports for a symlink and its target. Empty when not listed.
         */
        std::string unique{};

        bool operator==(ListingEntry const&) const = default;
    };

    /**
     * @brief Bounds on a listing, so a broken or hostile server cannot exhaust memory.
     */
    struct ListingLimits
    {
        /** @brief Bytes of one line, its line end included. Longer lines are skipped as unreadable. */
        std::size_t lineLength{64u * 1024u};
        /** @brief Entries kept of one listing, later ones are counted as dropped. */
        std::size_t entryCount{1'000'000u};
    };

    /**
     * @brief The entries of a listing and what the parser left out. Names of children are checked: an entry named
     *        "." or ".." is never a child and is left out without being counted.
     */
    struct Listing
    {
        std::vector<ListingEntry> entries{};
        /**
         * @brief Lines in no format the parser reads, or longer than ListingLimits::lineLength. Blank lines and the
         *        "total" line of LIST are not counted.
         */
        std::size_t unreadableLines{0u};
        /**
         * @brief Facts of MLSx entries whose value could not be read, like a modify that is no time-val. The entry
         *        is kept without them.
         */
        std::size_t unreadableFacts{0u};
        /**
         * @brief Entries whose name names no child of the listed directory: empty, with '/', NUL or CR, or longer
         *        than ListingParser::maximumNameLength.
         */
        std::size_t refusedNames{0u};
        /**
         * @brief Entries with the name of an earlier one, which is kept. RFC 3659 7.5.1.6 lists an item that is of
         *        several types once per type.
         */
        std::size_t repeatedNames{0u};
        /** @brief Entries past ListingLimits::entryCount, read but not kept. */
        std::size_t droppedEntries{0u};

        bool operator==(Listing const&) const = default;
    };

    /**
     * @brief Reads the data of MLSD, LIST or NLST as it arrives. Lines end in CRLF or LF, whatever TYPE is set.
     */
    class ListingParser
    {
      public:
        /** @brief Longer names are refused, file systems allow 255 bytes or 255 UTF-16 units. */
        static constexpr std::size_t maximumNameLength = 1024u;

        /**
         * @brief Reads MLSD (RFC 3659 7): facts in any case, one space, then the name. cdir and pdir entries are
         *        left out.
         */
        static ListingParser machineListing(ListingLimits limits = {});

        /**
         * @brief Reads LIST in the format of Unix "ls -l" and in the DOS format of IIS, line by line.
         *
         * - Unix: the name starts one space after the time or year, so leading spaces belong to it. A symlink is
         *   split at its first " -> ". An entry with a time of day but no year lies in the latest year that puts it at
         *   most a day after @p today, where the day absorbs the time zone of the server. Owner and group stay empty
         *   when a number that is a link count or the id of the owner comes before at most one other column.
         * - DOS: the spaces before the name are left out.
         *
         * @param today The current date of the client, UTC.
         */
        static ListingParser list(std::chrono::sys_days today, ListingLimits limits = {});

        /**
         * @brief Reads NLST: one name per line, of unknown type.
         *
         * @param listedPath The argument of NLST, empty for none. Servers that list each name behind it lose the
         *        prefix.
         */
        static ListingParser nameList(std::string_view listedPath, ListingLimits limits = {});

        /**
         * @brief Reads the complete lines of @p bytes and keeps the rest for the next call.
         */
        void feed(std::string_view bytes);

        /**
         * @brief Reads the last line, which needs no line end, and returns the listing. The parser starts over
         *        afterwards.
         */
        Listing finish();

      private:
        enum class Format : std::uint8_t
        {
            Machine,
            List,
            NameList,
        };

        ListingParser(Format format, ListingLimits limits, std::chrono::sys_days today, std::string listedPrefix);

        void readLine(std::string_view line);
        void keep(ListingEntry entry);

        Format format_;
        ListingLimits limits_;
        std::chrono::sys_days today_;
        std::string listedPrefix_;
        std::string pending_{};
        bool skippingLongLine_{false};
        Listing listing_{};
    };

    /**
     * @brief The entry of the 250 reply to MLST (RFC 3659 7.2): the line that starts with a space. Its name is the
     *        path as the server chose to write it, often a full path, and cdir names a directory.
     *
     * @return The replyError() of another code, an unreadableReply() for a 250 without a readable entry.
     */
    std::expected<ListingEntry, RemoteIo::RemoteError> parseMachineListReply(Reply const& reply);
}

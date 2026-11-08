#include <ftp/protocol/listing.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
    using namespace FileTransferProtocol;

    constexpr std::chrono::sys_days today{std::chrono::year{2026} / 10 / 28};
    /** @brief Small enough that inputs reach them. */
    constexpr ListingLimits smallLimits{.lineLength = 64u, .entryCount = 4u};

    void check(bool condition)
    {
        if (!condition)
            std::abort();
    }

    Listing parsedInPieces(ListingParser parser, std::string_view bytes, std::size_t pieceSize)
    {
        for (std::size_t position = 0u; position < bytes.size(); position += pieceSize)
            parser.feed(bytes.substr(position, pieceSize));
        return parser.finish();
    }

    /**
     * @brief Every entry names a child once, the limits hold and every line gave at most one entry or count.
     */
    void checkListing(Listing const& listing, std::string_view bytes, ListingLimits limits)
    {
        check(listing.entries.size() <= limits.entryCount);
        std::unordered_set<std::string_view> names{};
        for (auto const& entry : listing.entries)
        {
            check(!entry.name.empty() && entry.name.size() <= ListingParser::maximumNameLength);
            check(entry.name.find_first_of(std::string_view{"/\0\r\n", 4u}) == std::string_view::npos);
            check(entry.name != "." && entry.name != "..");
            check(names.insert(entry.name).second);
        }
        const auto lines = static_cast<std::size_t>(std::ranges::count(bytes, '\n')) + 1u;
        check(
            listing.entries.size() + listing.unreadableLines + listing.refusedNames + listing.repeatedNames +
                listing.droppedEntries <=
            lines
        );
    }

    template <typename MakeParserT>
    void checkFormat(MakeParserT makeParser, std::string_view bytes)
    {
        for (const auto limits : {ListingLimits{}, smallLimits})
        {
            const auto whole = parsedInPieces(makeParser(limits), bytes, std::max<std::size_t>(bytes.size(), 1u));
            checkListing(whole, bytes, limits);
            check(parsedInPieces(makeParser(limits), bytes, 1u) == whole);
            check(parsedInPieces(makeParser(limits), bytes, 7u) == whole);
        }
    }

    void checkMachineListReply(std::string_view bytes)
    {
        Reply reply{.code = 250, .lines = {"250-Begin"}};
        std::size_t position = 0u;
        while (position < bytes.size())
        {
            const auto lineEnd = std::min(bytes.find('\n', position), bytes.size());
            reply.lines.emplace_back(bytes.substr(position, lineEnd - position));
            position = lineEnd + 1u;
        }
        reply.lines.emplace_back("250 End");
        static_cast<void>(parseMachineListReply(reply));
    }
}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    const std::string_view bytes{reinterpret_cast<char const*>(data), size};
    checkFormat(ListingParser::machineListing, bytes);
    checkFormat(
        [](ListingLimits limits)
        {
            return ListingParser::list(today, limits);
        },
        bytes
    );
    checkFormat(
        [](ListingLimits limits)
        {
            return ListingParser::nameList("dir", limits);
        },
        bytes
    );
    checkMachineListReply(bytes);
    return 0;
}

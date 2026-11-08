#include "listing_lines.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace FileTransferProtocol::Detail
{
    namespace
    {
        /**
         * @brief The facts of one entry while they are read, owners and sizes from several facts.
         */
        struct Facts
        {
            ListingEntry entry{};
            std::optional<std::uint64_t> size{};
            std::optional<std::uint64_t> directorySize{};
            std::string ownerName{};
            std::string ownerId{};
            std::string groupName{};
            std::string groupId{};
            bool currentOrParent{false};
            std::size_t unreadable{0u};
        };

        /**
         * @brief Reads a type value (RFC 3659 7.5.1), in any case. Unknown types, OS ones included, stay Unknown.
         */
        void readType(std::string_view value, Facts& facts)
        {
            using SharedData::FileType;
            constexpr std::string_view unixType = "OS.unix=";
            constexpr std::string_view symlinkWithTarget = "slink:";

            if (equalIgnoringCase(value, "file"))
                facts.entry.type = FileType::Regular;
            else if (equalIgnoringCase(value, "dir"))
                facts.entry.type = FileType::Directory;
            else if (equalIgnoringCase(value, "cdir") || equalIgnoringCase(value, "pdir"))
            {
                facts.entry.type = FileType::Directory;
                facts.currentOrParent = true;
            }
            else if (startsWithIgnoringCase(value, unixType))
            {
                const auto kind = value.substr(unixType.size());
                if (equalIgnoringCase(kind, "symlink"))
                    facts.entry.type = FileType::Symlink;
                else if (startsWithIgnoringCase(kind, symlinkWithTarget))
                {
                    facts.entry.type = FileType::Symlink;
                    // pure-ftpd sends no target.
                    if (kind.size() > symlinkWithTarget.size())
                        facts.entry.linkTarget = std::string{kind.substr(symlinkWithTarget.size())};
                }
                else if (startsWithIgnoringCase(kind, "chr-"))
                    facts.entry.type = FileType::CharDevice;
                else if (startsWithIgnoringCase(kind, "blk-"))
                    facts.entry.type = FileType::BlockDevice;
            }
        }

        bool isOctalDigit(char character)
        {
            return character >= '0' && character <= '7';
        }

        /**
         * @brief Reads UNIX.mode, octal with or without a leading 0, or 0o as pyftpdlib writes it.
         */
        std::optional<std::filesystem::perms> modeOf(std::string_view value)
        {
            if (startsWithIgnoringCase(value, "0o"))
                value.remove_prefix(2u);
            if (value.empty() || !std::ranges::all_of(value, isOctalDigit))
                return std::nullopt;
            unsigned mode = 0u;
            std::size_t position = 0u;
            while (mode <= 07777u && position < value.size())
                mode = mode * 8u + static_cast<unsigned>(value[position++] - '0');
            if (mode > 07777u)
                return std::nullopt;
            return static_cast<std::filesystem::perms>(mode);
        }

        /**
         * @brief Reads one fact into @p facts. Facts the client has no use for are ignored (RFC 3659 7.6.2).
         */
        void readFact(std::string_view name, std::string_view value, Facts& facts)
        {
            const auto unreadableUnless = [&facts](bool readable)
            {
                if (!readable)
                    ++facts.unreadable;
            };

            if (equalIgnoringCase(name, "type"))
                readType(value, facts);
            else if (equalIgnoringCase(name, "size"))
            {
                facts.size = decimalOf<std::uint64_t>(value);
                unreadableUnless(facts.size.has_value());
            }
            else if (equalIgnoringCase(name, "sizd"))
            {
                facts.directorySize = decimalOf<std::uint64_t>(value);
                unreadableUnless(facts.directorySize.has_value());
            }
            else if (equalIgnoringCase(name, "modify"))
            {
                const auto modified = parseTimeVal(value);
                if (modified)
                    facts.entry.modified = *modified;
                unreadableUnless(modified.has_value());
            }
            else if (equalIgnoringCase(name, "unique"))
                facts.entry.unique = std::string{value};
            else if (equalIgnoringCase(name, "UNIX.mode"))
            {
                const auto mode = modeOf(value);
                if (mode)
                    facts.entry.permissions = *mode;
                unreadableUnless(mode.has_value());
            }
            else if (equalIgnoringCase(name, "UNIX.ownername"))
                facts.ownerName = std::string{value};
            else if (equalIgnoringCase(name, "UNIX.owner") || equalIgnoringCase(name, "UNIX.uid"))
                facts.ownerId = std::string{value};
            else if (equalIgnoringCase(name, "UNIX.groupname"))
                facts.groupName = std::string{value};
            else if (equalIgnoringCase(name, "UNIX.group") || equalIgnoringCase(name, "UNIX.gid"))
                facts.groupId = std::string{value};
        }

        ListingEntry entryOf(Facts facts)
        {
            auto entry = std::move(facts.entry);
            entry.size = facts.size ? facts.size : facts.directorySize;
            entry.owner = facts.ownerName.empty() ? std::move(facts.ownerId) : std::move(facts.ownerName);
            entry.group = facts.groupName.empty() ? std::move(facts.groupId) : std::move(facts.groupName);
            return entry;
        }
    }

    std::optional<ListingLine> readMachineListingLine(std::string_view line, CurrentAndParent currentAndParent)
    {
        // Facts hold no space (RFC 3659 7.4), the first one ends them.
        const auto separator = line.find(' ');
        if (separator == std::string_view::npos)
            return std::nullopt;
        const auto factText = line.substr(0u, separator);
        if (!factText.empty() && factText.back() != ';')
            return std::nullopt;

        Facts facts{.entry = ListingEntry{.name = std::string{line.substr(separator + 1u)}}};
        std::unordered_set<std::string> seenNames{};
        std::size_t position = 0u;
        while (position < factText.size())
        {
            const auto end = factText.find(';', position);
            const auto fact = factText.substr(position, end - position);
            position = end + 1u;

            const auto equals = fact.find('=');
            if (equals == 0u || equals == std::string_view::npos)
                return std::nullopt;
            const auto name = fact.substr(0u, equals);
            if (!seenNames.insert(Utility::Algorithm::toLowerCase(std::string{name})).second)
                return std::nullopt;
            readFact(name, fact.substr(equals + 1u), facts);
        }

        const auto unreadableFacts = facts.unreadable;
        if (facts.currentOrParent && currentAndParent == CurrentAndParent::Skip)
            return ListingLine{.unreadableFacts = unreadableFacts};
        return ListingLine{.entry = entryOf(std::move(facts)), .unreadableFacts = unreadableFacts};
    }
}

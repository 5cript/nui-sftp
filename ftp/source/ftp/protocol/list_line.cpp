#include "listing_lines.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace FileTransferProtocol::Detail
{
    namespace
    {
        using SharedData::FileType;
        using std::filesystem::perms;

        struct Token
        {
            std::string_view text{};
            /** @brief Where the token ends in its line. */
            std::size_t end{0u};
        };

        /**
         * @brief The first words of a line that spaces separate, as many as a Unix line has up to its date: mode, link
         *        count, owner, group, two device numbers, month, day and time or year.
         */
        struct LeadingTokens
        {
            std::array<Token, 9u> tokens{};
            std::size_t count{0u};

            std::span<Token const> span() const
            {
                return {tokens.data(), count};
            }
        };

        LeadingTokens leadingTokensOf(std::string_view line)
        {
            LeadingTokens leading{};
            auto start = line.find_first_not_of(' ');
            while (leading.count < leading.tokens.size() && start != std::string_view::npos)
            {
                const auto end = std::min(line.find(' ', start), line.size());
                leading.tokens[leading.count++] = Token{.text = line.substr(start, end - start), .end = end};
                start = line.find_first_not_of(' ', end);
            }
            return leading;
        }

        bool isDecimal(std::string_view text)
        {
            return !text.empty() && std::ranges::all_of(text, isDigit);
        }

        struct Mode
        {
            FileType type{FileType::Unknown};
            perms permissions{perms::none};
        };

        std::optional<FileType> typeOfLetter(char letter)
        {
            switch (letter)
            {
                case '-':
                    return FileType::Regular;
                case 'd':
                    return FileType::Directory;
                case 'l':
                    return FileType::Symlink;
                case 'c':
                    return FileType::CharDevice;
                case 'b':
                    return FileType::BlockDevice;
                case 'p':
                    return FileType::Fifo;
                case 's':
                    return FileType::Socket;
                case 'D':
                    // A Solaris door.
                    return FileType::Special;
                default:
                    return std::nullopt;
            }
        }

        /**
         * @brief The bits of one rwx triplet and the special bit its execute letter shows.
         */
        struct Triplet
        {
            perms read;
            perms write;
            perms execute;
            perms special;
            /** @brief The execute letter for execute and special, 's' or 't'. */
            char executableSpecial;
            /** @brief The execute letter for special alone, 'S' or 'T'. */
            char specialOnly;
        };

        /**
         * @brief Reads the mode of "ls -l" like "drwxr-sr-x", with an optional ACL or context marker after it.
         */
        std::optional<Mode> modeOf(std::string_view text)
        {
            constexpr std::size_t letters = 10u;
            const auto marked =
                text.size() == letters + 1u && (text.back() == '+' || text.back() == '.' || text.back() == '@');
            if (text.size() != letters && !marked)
                return std::nullopt;
            const auto type = typeOfLetter(text.front());
            if (!type)
                return std::nullopt;

            constexpr std::array<Triplet, 3u> triplets{{
                {perms::owner_read, perms::owner_write, perms::owner_exec, perms::set_uid, 's', 'S'},
                {perms::group_read, perms::group_write, perms::group_exec, perms::set_gid, 's', 'S'},
                {perms::others_read, perms::others_write, perms::others_exec, perms::sticky_bit, 't', 'T'},
            }};
            Mode mode{.type = *type};
            std::size_t index = 0u;
            bool readable = true;
            while (readable && index < triplets.size())
            {
                auto const& triplet = triplets[index];
                const auto read = text[1u + index * 3u];
                const auto write = text[2u + index * 3u];
                const auto execute = text[3u + index * 3u];
                readable = (read == 'r' || read == '-') && (write == 'w' || write == '-');
                if (read == 'r')
                    mode.permissions |= triplet.read;
                if (write == 'w')
                    mode.permissions |= triplet.write;
                if (execute == 'x' || execute == triplet.executableSpecial)
                    mode.permissions |= triplet.execute;
                if (execute == triplet.executableSpecial || execute == triplet.specialOnly)
                    mode.permissions |= triplet.special;
                readable = readable &&
                    (execute == 'x' || execute == '-' || execute == triplet.executableSpecial ||
                        execute == triplet.specialOnly);
                ++index;
            }
            if (!readable)
                return std::nullopt;
            return mode;
        }

        std::optional<unsigned> monthOf(std::string_view text)
        {
            constexpr std::array<std::string_view, 12u> months{
                "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            const auto found = std::ranges::find_if(
                months,
                [text](std::string_view month)
                {
                    return equalIgnoringCase(month, text);
                }
            );
            if (found == months.end())
                return std::nullopt;
            return static_cast<unsigned>(found - months.begin()) + 1u;
        }

        std::optional<unsigned> dayOf(std::string_view text)
        {
            const auto day = text.size() <= 2u ? decimalOf<unsigned>(text) : std::nullopt;
            if (!day || *day < 1u || *day > 31u)
                return std::nullopt;
            return day;
        }

        /**
         * @brief The time of day "H:MM" or "HH:MM" from @p text.
         */
        std::optional<std::chrono::minutes> clockOf(std::string_view text)
        {
            const auto colon = text.find(':');
            if (colon == std::string_view::npos || colon == 0u || colon > 2u || text.size() != colon + 3u)
                return std::nullopt;
            const auto hour = decimalOf<unsigned>(text.substr(0u, colon));
            const auto minute = decimalOf<unsigned>(text.substr(colon + 1u));
            if (!hour || !minute || *hour > 23u || *minute > 59u)
                return std::nullopt;
            return std::chrono::hours{*hour} + std::chrono::minutes{*minute};
        }

        std::optional<int> yearOf(std::string_view text)
        {
            if (text.size() != 4u)
                return std::nullopt;
            return decimalOf<int>(text);
        }

        /**
         * @brief The date of a month and day that LIST printed with a time of day: in the latest of the year after,
         *        the year of and the year before @p today that puts it at most a day after @p today.
         */
        std::optional<std::chrono::local_days>
        recentDate(std::chrono::month month, std::chrono::day day, std::chrono::sys_days today)
        {
            using namespace std::chrono;
            const auto currentYear = year_month_day{today}.year();
            std::optional<local_days> date{};
            auto candidateYear = currentYear + years{1};
            while (!date && candidateYear >= currentYear - years{1})
            {
                const year_month_day candidate{candidateYear / month / day};
                if (candidate.ok() && sys_days{candidate} <= today + days{1})
                    date = local_days{candidate};
                candidateYear -= years{1};
            }
            return date;
        }

        /**
         * @brief Where the date of a Unix line starts: a month, a day, a time or year, after a size or a device minor.
         */
        std::optional<std::size_t> dateIndexOf(std::span<Token const> tokens)
        {
            std::optional<std::size_t> found{};
            std::size_t index = 2u;
            while (!found && index + 2u < tokens.size())
            {
                const auto sizeOrDevice = tokens[index - 1u].text;
                const auto deviceComma = sizeOrDevice.find(',');
                const auto sizeLike = isDecimal(sizeOrDevice) ||
                    (deviceComma != std::string_view::npos && isDecimal(sizeOrDevice.substr(0u, deviceComma)) &&
                        isDecimal(sizeOrDevice.substr(deviceComma + 1u)));
                const auto timeOrYear = tokens[index + 2u].text;
                if (sizeLike && monthOf(tokens[index].text) && dayOf(tokens[index + 1u].text) &&
                    (clockOf(timeOrYear) || yearOf(timeOrYear)))
                    found = index;
                ++index;
            }
            return found;
        }

        bool endsDeviceMajor(std::string_view text)
        {
            return text.size() > 1u && text.back() == ',' && isDecimal(text.substr(0u, text.size() - 1u));
        }

        /**
         * @brief Where the size of a Unix line starts, or the device numbers of a device, and the size of an entry
         *        that is no device.
         */
        struct SizeColumn
        {
            std::size_t index{0u};
            std::optional<std::uint64_t> size{};
        };

        std::optional<SizeColumn> sizeColumnOf(std::span<Token const> tokens, std::size_t dateIndex, FileType type)
        {
            const auto index = dateIndex - 1u;
            const auto sizeText = tokens[index].text;
            if (type == FileType::CharDevice || type == FileType::BlockDevice)
            {
                const auto majorApart =
                    sizeText.find(',') == std::string_view::npos && endsDeviceMajor(tokens[index - 1u].text);
                return SizeColumn{.index = majorApart ? index - 1u : index};
            }
            return decimalOf<std::uint64_t>(sizeText).transform(
                [index](std::uint64_t size)
                {
                    return SizeColumn{.index = index, .size = size};
                }
            );
        }

        struct OwnerAndGroup
        {
            std::string_view owner{};
            std::string_view group{};
        };

        /**
         * @brief The owner and the group among the tokens between the mode and the size, which are a link count, the
         *        owner and the group, each of them optional. Both stay unknown when fewer than three start with a
         *        number, which is a link count or the id of the owner.
         *
         * @return Nothing for more than three tokens.
         */
        std::optional<OwnerAndGroup> ownerAndGroupOf(std::span<Token const> middle)
        {
            if (middle.size() > 3u)
                return std::nullopt;
            if (middle.size() == 3u)
                return OwnerAndGroup{.owner = middle[1u].text, .group = middle[2u].text};
            if (middle.empty() || isDecimal(middle.front().text))
                return OwnerAndGroup{};
            return OwnerAndGroup{.owner = middle.front().text, .group = middle.size() == 2u ? middle.back().text : ""};
        }

        /**
         * @brief The time of the month, the day and the time or year that start at @p dateIndex, see dateIndexOf.
         */
        std::optional<ListedTime>
        listedTimeOf(std::span<Token const> tokens, std::size_t dateIndex, std::chrono::sys_days today)
        {
            const auto month = std::chrono::month{*monthOf(tokens[dateIndex].text)};
            const auto day = std::chrono::day{*dayOf(tokens[dateIndex + 1u].text)};
            const auto timeOrYear = tokens[dateIndex + 2u].text;
            if (const auto clock = clockOf(timeOrYear))
            {
                return recentDate(month, day, today)
                    .transform(
                        [clock](std::chrono::local_days date)
                        {
                            return ListedTime{.time = date + *clock, .precision = ListedTimePrecision::Minute};
                        }
                    );
            }
            const std::chrono::year_month_day date{std::chrono::year{*yearOf(timeOrYear)} / month / day};
            if (!date.ok())
                return std::nullopt;
            return ListedTime{.time = std::chrono::local_days{date}, .precision = ListedTimePrecision::Day};
        }

        /**
         * @brief Splits the name of a symlink at its first " -> " into the name and the target.
         */
        void splitLinkTarget(ListingEntry& entry)
        {
            constexpr std::string_view arrow = " -> ";
            const auto arrowStart = entry.name.find(arrow);
            if (arrowStart == std::string::npos)
                return;
            entry.linkTarget = entry.name.substr(arrowStart + arrow.size());
            entry.name.resize(arrowStart);
        }

        /**
         * @brief Reads a line of "ls -l": mode, an optional link count, owner and group when listed, the size or the
         *        device numbers, the date, then the name one space after it.
         */
        std::optional<ListingLine>
        readUnixLine(std::string_view line, std::span<Token const> tokens, std::chrono::sys_days today)
        {
            const auto mode = tokens.empty() ? std::nullopt : modeOf(tokens.front().text);
            const auto dateIndex = mode ? dateIndexOf(tokens) : std::nullopt;
            if (!dateIndex)
                return std::nullopt;
            const auto sizeColumn = sizeColumnOf(tokens, *dateIndex, mode->type);
            if (!sizeColumn)
                return std::nullopt;
            const auto ownerAndGroup = ownerAndGroupOf(tokens.subspan(1u, sizeColumn->index - 1u));
            const auto listedTime = listedTimeOf(tokens, *dateIndex, today);
            const auto nameStart = tokens[*dateIndex + 2u].end + 1u;
            if (!ownerAndGroup || !listedTime || nameStart >= line.size())
                return std::nullopt;

            ListingEntry entry{
                .name = std::string{line.substr(nameStart)},
                .type = mode->type,
                .size = sizeColumn->size,
                .listedTime = listedTime,
                .permissions = mode->permissions,
                .owner = std::string{ownerAndGroup->owner},
                .group = std::string{ownerAndGroup->group},
            };
            if (entry.type == FileType::Symlink)
                splitLinkTarget(entry);
            return ListingLine{.entry = std::move(entry)};
        }

        /**
         * @brief The date "MM-DD-YY" or "MM-DD-YYYY" of a DOS line. Two-digit years from 69 on are 19xx, as for
         *        POSIX %y.
         */
        std::optional<std::chrono::local_days> dosDateOf(std::string_view text)
        {
            if ((text.size() != 8u && text.size() != 10u) || text[2u] != '-' || text[5u] != '-')
                return std::nullopt;
            const auto month = decimalOf<unsigned>(text.substr(0u, 2u));
            const auto day = decimalOf<unsigned>(text.substr(3u, 2u));
            auto year = decimalOf<int>(text.substr(6u));
            if (!month || !day || !year)
                return std::nullopt;
            if (text.size() == 8u)
                *year += *year >= 69 ? 1900 : 2000;
            const std::chrono::year_month_day date{std::chrono::year{*year} / *month / *day};
            if (!date.ok())
                return std::nullopt;
            return std::chrono::local_days{date};
        }

        /**
         * @brief The time "HH:MM" of a DOS line, with AM or PM, or on a 24-hour clock.
         */
        std::optional<std::chrono::minutes> dosClockOf(std::string_view text)
        {
            using std::chrono::hours;
            const auto morning = endsWithIgnoringCase(text, "AM");
            const auto afternoon = endsWithIgnoringCase(text, "PM");
            if (!morning && !afternoon)
                return text.size() == 5u ? clockOf(text) : std::nullopt;
            const auto clock = text.size() == 7u ? clockOf(text.substr(0u, 5u)) : std::nullopt;
            if (!clock)
                return std::nullopt;
            const auto hour = std::chrono::floor<hours>(*clock);
            if (hour < hours{1} || hour > hours{12})
                return std::nullopt;
            // 12 AM is midnight, 12 PM noon.
            const auto sinceMidnightOrNoon = *clock - (hour == hours{12} ? hours{12} : hours{0});
            return sinceMidnightOrNoon + (afternoon ? hours{12} : hours{0});
        }

        /**
         * @brief Reads a line of the DOS format of IIS: date, time, "<DIR>" or the size, then the name.
         */
        std::optional<ListingLine> readDosLine(std::string_view line, std::span<Token const> tokens)
        {
            if (tokens.size() < 3u)
                return std::nullopt;
            const auto date = dosDateOf(tokens[0u].text);
            const auto clock = dosClockOf(tokens[1u].text);
            if (!date || !clock)
                return std::nullopt;

            ListingEntry entry{
                .listedTime = ListedTime{.time = *date + *clock, .precision = ListedTimePrecision::Minute}
            };
            if (equalIgnoringCase(tokens[2u].text, "<DIR>"))
                entry.type = FileType::Directory;
            else
            {
                entry.type = FileType::Regular;
                entry.size = decimalOf<std::uint64_t>(tokens[2u].text);
                if (!entry.size)
                    return std::nullopt;
            }
            const auto nameStart = line.find_first_not_of(' ', tokens[2u].end);
            if (nameStart == std::string_view::npos)
                return std::nullopt;
            entry.name = std::string{line.substr(nameStart)};
            return ListingLine{.entry = std::move(entry)};
        }

        bool isTotalLine(std::span<Token const> tokens)
        {
            return tokens.size() == 2u && equalIgnoringCase(tokens[0u].text, "total") && isDecimal(tokens[1u].text);
        }
    }

    std::optional<ListingLine> readListLine(std::string_view line, std::chrono::sys_days today)
    {
        const auto leading = leadingTokensOf(line);
        const auto tokens = leading.span();
        if (isTotalLine(tokens))
            return ListingLine{};
        if (!line.empty() && isDigit(line.front()))
            return readDosLine(line, tokens);
        return readUnixLine(line, tokens, today);
    }
}

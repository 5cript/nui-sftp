#include "listing_lines.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
         * @brief The first @p count words of @p line that spaces separate, fewer when the line has fewer.
         */
        std::vector<Token> leadingTokens(std::string_view line, std::size_t count)
        {
            std::vector<Token> tokens{};
            auto start = line.find_first_not_of(' ');
            while (tokens.size() < count && start != std::string_view::npos)
            {
                const auto end = std::min(line.find(' ', start), line.size());
                tokens.push_back(Token{.text = line.substr(start, end - start), .end = end});
                start = line.find_first_not_of(' ', end);
            }
            return tokens;
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

            constexpr std::array<Triplet, 3> triplets{{
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
            constexpr std::array<std::string_view, 12> months{
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
         * @brief The date of a month and day that LIST printed with a time of day: in the year of @p today, unless
         *        that is more than a day ahead of it or does not exist, then in the year before.
         */
        std::optional<std::chrono::local_days>
        recentDate(std::chrono::month month, std::chrono::day day, std::chrono::sys_days today)
        {
            using namespace std::chrono;
            const year_month_day current{today};
            const year_month_day candidate{current.year() / month / day};
            if (candidate.ok() && sys_days{candidate} <= today + days{1})
                return local_days{candidate};
            const year_month_day earlier{(current.year() - years{1}) / month / day};
            if (!earlier.ok())
                return std::nullopt;
            return local_days{earlier};
        }

        /**
         * @brief Where the date of a Unix line starts: a month, a day, a time or year, after a size or a device minor.
         */
        std::optional<std::size_t> dateIndexOf(std::vector<Token> const& tokens)
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
         * @brief Reads a line of "ls -l": mode, an optional link count, owner and group when listed, the size or the
         *        device numbers, the date, then the name one space after it.
         */
        std::optional<ListingLine> readUnixLine(std::string_view line, std::chrono::sys_days today)
        {
            // Mode, link count, owner, group, two device numbers, month, day and time or year at most.
            constexpr std::size_t tokensUpToDate = 9u;
            const auto tokens = leadingTokens(line, tokensUpToDate);
            if (tokens.empty())
                return std::nullopt;
            const auto mode = modeOf(tokens.front().text);
            const auto dateIndex = mode ? dateIndexOf(tokens) : std::nullopt;
            if (!dateIndex)
                return std::nullopt;

            ListingEntry entry{.type = mode->type, .permissions = mode->permissions};
            auto sizeIndex = *dateIndex - 1u;
            const auto sizeText = tokens[sizeIndex].text;
            if (sizeText.find(',') == std::string_view::npos && !endsDeviceMajor(tokens[sizeIndex - 1u].text))
            {
                entry.size = decimalOf<std::uint64_t>(sizeText);
                if (!entry.size)
                    return std::nullopt;
            }
            else if (sizeText.find(',') == std::string_view::npos)
                --sizeIndex;

            // Between the mode and the size: a link count, the owner and the group, each may be missing.
            std::vector<std::string_view> middle{};
            for (std::size_t index = 1u; index < sizeIndex; ++index)
                middle.push_back(tokens[index].text);
            if (middle.size() > 3u)
                return std::nullopt;
            if (middle.size() == 3u || (middle.size() == 2u && isDecimal(middle.front())) ||
                (middle.size() == 1u && isDecimal(middle.front())))
                middle.erase(middle.begin());
            if (!middle.empty())
                entry.owner = std::string{middle.front()};
            if (middle.size() == 2u)
                entry.group = std::string{middle.back()};

            const auto month = std::chrono::month{*monthOf(tokens[*dateIndex].text)};
            const auto day = std::chrono::day{*dayOf(tokens[*dateIndex + 1u].text)};
            auto const& timeOrYear = tokens[*dateIndex + 2u];
            if (const auto clock = clockOf(timeOrYear.text))
            {
                const auto date = recentDate(month, day, today);
                if (!date)
                    return std::nullopt;
                entry.listedTime = ListedTime{.time = *date + *clock, .precision = ListedTimePrecision::Minute};
            }
            else
            {
                const std::chrono::year_month_day date{std::chrono::year{*yearOf(timeOrYear.text)} / month / day};
                if (!date.ok())
                    return std::nullopt;
                entry.listedTime =
                    ListedTime{.time = std::chrono::local_days{date}, .precision = ListedTimePrecision::Day};
            }

            const auto nameStart = timeOrYear.end + 1u;
            if (nameStart >= line.size())
                return std::nullopt;
            auto name = line.substr(nameStart);
            if (entry.type == FileType::Symlink)
            {
                constexpr std::string_view arrow = " -> ";
                const auto arrowStart = name.find(arrow);
                if (arrowStart != std::string_view::npos)
                {
                    entry.linkTarget = std::string{name.substr(arrowStart + arrow.size())};
                    name = name.substr(0u, arrowStart);
                }
            }
            entry.name = std::string{name};
            return ListingLine{.entry = std::move(entry)};
        }

        /**
         * @brief The date "MM-DD-YY" or "MM-DD-YYYY" of a DOS line. Two-digit years from 69 on are 19xx, as for
         *        POSIX %y.
         */
        std::optional<std::chrono::local_days> dosDateOf(std::string_view text)
        {
            if ((text.size() != 8u && text.size() != 10u) || text[2] != '-' || text[5] != '-')
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
            const auto hour = clock ? std::chrono::floor<hours>(*clock) : hours{0};
            if (hour < hours{1} || hour > hours{12})
                return std::nullopt;
            // 12 AM is midnight, 12 PM noon.
            const auto sinceMidnightOrNoon = *clock - (hour == hours{12} ? hours{12} : hours{0});
            return sinceMidnightOrNoon + (afternoon ? hours{12} : hours{0});
        }

        /**
         * @brief Reads a line of the DOS format of IIS: date, time, "<DIR>" or the size, then the name.
         */
        std::optional<ListingLine> readDosLine(std::string_view line)
        {
            const auto tokens = leadingTokens(line, 3u);
            if (tokens.size() < 3u)
                return std::nullopt;
            const auto date = dosDateOf(tokens[0].text);
            const auto clock = dosClockOf(tokens[1].text);
            if (!date || !clock)
                return std::nullopt;

            ListingEntry entry{
                .listedTime = ListedTime{.time = *date + *clock, .precision = ListedTimePrecision::Minute}
            };
            if (equalIgnoringCase(tokens[2].text, "<DIR>"))
                entry.type = FileType::Directory;
            else
            {
                entry.type = FileType::Regular;
                entry.size = decimalOf<std::uint64_t>(tokens[2].text);
                if (!entry.size)
                    return std::nullopt;
            }
            const auto nameStart = line.find_first_not_of(' ', tokens[2].end);
            if (nameStart == std::string_view::npos)
                return std::nullopt;
            entry.name = std::string{line.substr(nameStart)};
            return ListingLine{.entry = std::move(entry)};
        }

        bool isTotalLine(std::string_view line)
        {
            const auto tokens = leadingTokens(line, 3u);
            return tokens.size() == 2u && equalIgnoringCase(tokens[0].text, "total") && isDecimal(tokens[1].text);
        }
    }

    std::optional<ListingLine> readListLine(std::string_view line, std::chrono::sys_days today)
    {
        if (isTotalLine(line))
            return ListingLine{};
        if (isDigit(line.front()))
            return readDosLine(line);
        return readUnixLine(line, today);
    }
}

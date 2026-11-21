#pragma once

#include <utility/describe.hpp>
#include <utility/directory_traversal.hpp>

#include <paths/relative_path.hpp>

#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include <algorithm>
#include <chrono>
#include <compare>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace SharedData
{
    /**
     * @brief How exact a modification time is. A listing may round it down, a compare of two times goes by the coarser
     *        of both.
     */
    enum class TimePrecision : std::uint8_t
    {
        Second,
        /** @brief FTP's LIST of an entry younger than about six months: a time of day without seconds. */
        Minute,
        /** @brief FTP's LIST of an older entry: a year instead of a time of day. */
        Day,
    };
    BOOST_DESCRIBE_ENUM(TimePrecision, Second, Minute, Day)

    /**
     * @brief How many seconds @p precision covers.
     */
    constexpr std::uint64_t secondsOf(TimePrecision precision)
    {
        switch (precision)
        {
            case TimePrecision::Minute:
                return 60u;
            case TimePrecision::Day:
                return 24u * 60u * 60u;
            case TimePrecision::Second:
                break;
        }
        return 1u;
    }

    /**
     * @brief How the time @p first compares to @p second, both seconds since the epoch, at the coarser of their
     *        precisions.
     */
    constexpr std::strong_ordering
    compareTimes(std::uint64_t first, TimePrecision firstPrecision, std::uint64_t second, TimePrecision secondPrecision)
    {
        const auto unit = std::max(secondsOf(firstPrecision), secondsOf(secondPrecision));
        return first / unit <=> second / unit;
    }

    enum class FileType : std::uint8_t
    {
        Unknown = 0,
        Regular = 1,
        Directory = 2,
        Symlink = 3,
        Special = 4,
        Socket = 5,
        CharDevice = 6,
        BlockDevice = 7,
        Fifo = 8
    };

    inline FileType fileTypeFromStdFilesystemType(std::filesystem::file_type type)
    {
        using enum std::filesystem::file_type;
        switch (type)
        {
            case (regular):
                return FileType::Regular;
            case (directory):
                return FileType::Directory;
            case (symlink):
                return FileType::Symlink;
            case (block):
                return FileType::BlockDevice;
            case (character):
                return FileType::CharDevice;
            case (fifo):
                return FileType::Fifo;
            case (socket):
                return FileType::Socket;
            case (unknown):
                return FileType::Unknown;
            default:
                return FileType::Unknown;
        }
    }

    inline std::string fileTypeToString(FileType type)
    {
        switch (type)
        {
            case FileType::Regular:
                return "Regular File";
            case FileType::Directory:
                return "Directory";
            case FileType::Symlink:
                return "Symbolic Link";
            case FileType::Special:
                return "Special File";
            case FileType::Socket:
                return "Socket";
            case FileType::CharDevice:
                return "Character Device";
            case FileType::BlockDevice:
                return "Block Device";
            case FileType::Fifo:
                return "FIFO";
            case FileType::Unknown:
            default:
                return "Unknown";
        }
    }

    struct DirectoryEntry
    {
        using FileType = SharedData::FileType;

        /**
         * @brief The name as listed, always valid UTF-8. A server or a hostile local name can be anything, like "..",
         *        "x/.." or empty, so it only becomes part of a path through @ref nameSegment. A listed name that was
         *        not UTF-8 is kept for display with U+FFFD for every invalid byte and @ref nameIsValidUtf8 false.
         *        Empty for the root of a walk and for stat results.
         */
        std::string name{};
        /**
         * @brief False when the listed name was not UTF-8 and @ref name only shows it: such an entry can be listed
         *        but never named in a path, so no action can hit a different item that is called like the display
         *        text.
         */
        bool nameIsValidUtf8{true};
        std::uint32_t flags{0};
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        /**
         * @brief The owner name, valid UTF-8 (U+FFFD for every invalid byte the server sent).
         */
        std::string owner{};
        /**
         * @brief The group name, valid UTF-8 (U+FFFD for every invalid byte the server sent).
         */
        std::string group{};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        std::uint64_t atime{0};
        std::uint32_t atimeNsec{0};
        std::uint64_t createTime{0};
        std::uint32_t createTimeNsec{0};
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        TimePrecision mtimePrecision{TimePrecision::Second};
        std::string acl{};
        /**
         * @brief For symlinks: the link literal readlink returned (not the resolved target), valid UTF-8 with U+FFFD
         *        for every invalid byte. nullopt when not a symlink or when unavailable. The sync diff compares by it,
         *        a transfer of the link reads the literal itself.
         */
        std::optional<std::string> linkTarget{};
        // For symlinks: the full entry of the symlink target (if known). nullptr means unknown.
        std::shared_ptr<DirectoryEntry> resolvedTarget{nullptr};

        /**
         * @brief The name as a path segment. Refused like Paths::PathSegment::make refuses it, and with InvalidUtf8
         *        when the listed name was not UTF-8.
         */
        std::expected<Paths::PathSegment, Paths::PathError> nameSegment() const
        {
            if (!nameIsValidUtf8)
                return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::InvalidUtf8, .input = name});
            return Paths::PathSegment::make(name);
        }

        bool isDirectory() const
        {
            return type == FileType::Directory;
        }
        // Returns true for directories and for symlinks whose target is a directory.
        bool isDirectoryLike() const
        {
            return type == FileType::Directory ||
                (type == FileType::Symlink && resolvedTarget && resolvedTarget->type == FileType::Directory);
        }
        bool isRegularFile() const
        {
            return type == FileType::Regular;
        }
        bool isSymlink() const
        {
            return type == FileType::Symlink;
        }
        bool isSpecial() const
        {
            return type == FileType::Special;
        }
        bool isUnknown() const
        {
            return type == FileType::Unknown;
        }
        bool isSocket() const
        {
            return type == FileType::Socket;
        }
        bool isCharDevice() const
        {
            return type == FileType::CharDevice;
        }
        bool isBlockDevice() const
        {
            return type == FileType::BlockDevice;
        }
        bool isFifo() const
        {
            return type == FileType::Fifo;
        }

        // Used for directory traversal. Avoids pointer instability in vector and unique_ptr
        std::optional<std::size_t> parent{std::nullopt};

        std::string readableATime() const
        {
            using namespace std::chrono;
            auto tp = system_clock::time_point{seconds{atime}};
            return fmt::format("{:%Y-%m-%d %H:%M:%S}", floor<seconds>(tp));
        }

        std::string readableMTime() const
        {
            using namespace std::chrono;
            auto tp = system_clock::time_point{seconds{mtime}};
            return fmt::format("{:%Y-%m-%d %H:%M:%S}", floor<seconds>(tp));
        }

        std::string readableCreateTime() const
        {
            using namespace std::chrono;
            auto tp = system_clock::time_point{seconds{createTime}};
            return fmt::format("{:%Y-%m-%d %H:%M:%S}", floor<seconds>(tp));
        }

        std::string lsStyleTypePermsUserGroup() const
        {
            using std::filesystem::perms;
            std::string cryptics = {
                [this]()
                {
                    switch (type)
                    {
                        case FileType::Regular:
                            return '-';
                        case FileType::Directory:
                            return 'd';
                        case FileType::Symlink:
                            return 'l';
                        case FileType::Socket:
                            return 's';
                        case FileType::CharDevice:
                            return 'c';
                        case FileType::BlockDevice:
                            return 'b';
                        case FileType::Fifo:
                            return 'p';
                        default:
                            return '?';
                    }
                }(),
                perms::none == (permissions & perms::owner_read) ? '-' : 'r',
                perms::none == (permissions & perms::owner_write) ? '-' : 'w',
                perms::none == (permissions & perms::owner_exec) ? '-' : 'x',
                perms::none == (permissions & perms::group_read) ? '-' : 'r',
                perms::none == (permissions & perms::group_write) ? '-' : 'w',
                perms::none == (permissions & perms::group_exec) ? '-' : 'x',
                perms::none == (permissions & perms::others_read) ? '-' : 'r',
                perms::none == (permissions & perms::others_write) ? '-' : 'w',
                perms::none == (permissions & perms::others_exec) ? '-' : 'x',
            };

            return fmt::format("{} {} {}", cryptics, owner, group);
        }
    };

    /**
     * @brief The path of @p entry below the root of its walk, following the parent indices. Empty for the root,
     *        which is the entry without a parent. Refused when a name on the way is no path segment or a parent index
     *        lies outside @p entries.
     */
    inline std::expected<Paths::RelativePath, Paths::PathError>
    relativePath(std::vector<DirectoryEntry> const& entries, DirectoryEntry const& entry)
    {
        return Utility::walkedRelativePath(entries, entry);
    }

    void to_json(nlohmann::json& j, DirectoryEntry const& entry);
    void from_json(nlohmann::json const& j, DirectoryEntry& entry);
}
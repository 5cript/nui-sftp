#pragma once

#include <tar_archive/error.hpp>
#include <shared_data/directory_entry.hpp>
#include <utility/describe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TarArchive
{
    /** @brief Size of every tar record in bytes. */
    inline constexpr std::size_t recordSize = 512u;

    /**
     * @brief Raw 512-byte tar record. Used both for USTAR file records and for PAX
     * extended-header records (typeflag 'x' / 'g').
     */
    using RawRecord = std::array<std::byte, recordSize>;

    /**
     * @brief POSIX ustar typeflag values. Values match the on-disk character codes.
     */
    enum class TypeFlag : char
    {
        RegularFileAlternate = '\0',
        RegularFile = '0',
        HardLink = '1',
        SymbolicLink = '2',
        CharacterSpecial = '3',
        BlockSpecial = '4',
        Directory = '5',
        FifoSpecial = '6',
        ContiguousFile = '7',
        GlobalExtendedHeader = 'g',
        ExtendedHeader = 'x'
    };

    BOOST_DESCRIBE_ENUM(
        TypeFlag,
        RegularFileAlternate,
        RegularFile,
        HardLink,
        SymbolicLink,
        CharacterSpecial,
        BlockSpecial,
        Directory,
        FifoSpecial,
        ContiguousFile,
        GlobalExtendedHeader,
        ExtendedHeader
    )

    /**
     * @brief Map a SharedData::FileType to the USTAR typeflag character.
     *
     * Returns UnsupportedFileType for the Unknown variant; all other variants map to a
     * defined USTAR typeflag.
     */
    std::expected<TypeFlag, TarError> typeFlagFromFileType(SharedData::FileType type) noexcept;

    /**
     * @brief Reverse mapping from USTAR typeflag to SharedData::FileType.
     */
    SharedData::FileType fileTypeFromTypeFlag(TypeFlag flag) noexcept;

    /**
     * @brief Computes the POSIX ustar header checksum for a raw record.
     *
     * The algorithm: sum all 512 bytes as unsigned chars, treating the 8 checksum bytes as
     * ASCII spaces. Used both when emitting and when verifying a record.
     */
    std::uint32_t calculateChecksum(RawRecord const& record) noexcept;

    /**
     * @brief Result of building the on-disk records for one logical archive entry.
     *
     * Records is the ordered sequence that must be written to the byte stream; for a plain
     * USTAR entry this is a single element, for entries requiring a PAX extended header it
     * contains the 'x' record(s) followed by the regular ustar record.
     */
    struct BuiltEntry
    {
        std::vector<RawRecord> records;
        std::uint64_t payloadSize{0};
    };

    /**
     * @brief Build the on-disk tar records for a DirectoryEntry, emitting PAX extended
     * headers when the entry's path, link target, or size exceed plain USTAR limits.
     */
    std::expected<BuiltEntry, TarError> buildRecords(SharedData::DirectoryEntry const& meta);

    /**
     * @brief Parsed representation of a single USTAR file record.
     *
     * Populated from the 512-byte on-disk bytes; represents one entry prior to applying any
     * PAX overrides that may have preceded it.
     */
    struct ParsedUstar
    {
        std::string name;
        std::string prefix;
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        std::uint64_t size{0};
        std::uint64_t mtime{0};
        TypeFlag typeflag{TypeFlag::RegularFile};
        std::string linkName;
        std::string uName;
        std::string gName;
        bool hasUstarMagic{false};

        /**
         * @brief Full entry name: prefix + '/' + name when prefix is non-empty, else name.
         */
        std::string fullName() const;
    };

    /**
     * @brief Returns true if the record is all zero bytes. Two consecutive zero records mark
     * end-of-archive.
     */
    bool isZeroRecord(RawRecord const& record) noexcept;

    /**
     * @brief Parse a 512-byte record into typed fields. Verifies the checksum; returns
     * ChecksumMismatch on corruption.
     */
    std::expected<ParsedUstar, TarError> parseRecord(RawRecord const& record);

    /**
     * @brief Overrides collected from any preceding PAX extended-header records.
     */
    struct PaxOverrides
    {
        std::optional<std::string> path;
        std::optional<std::string> linkPath;
        std::optional<std::uint64_t> size;
        std::optional<std::uint64_t> mtime;
        std::optional<std::uint32_t> uid;
        std::optional<std::uint32_t> gid;
        std::optional<std::string> uName;
        std::optional<std::string> gName;
    };

    /**
     * @brief Parse a PAX extended-header payload (raw concatenated "len key=value\n" records)
     * into a PaxOverrides structure. Unknown keys are silently ignored.
     */
    std::expected<PaxOverrides, TarError> parsePaxPayload(std::span<std::byte const> payload);

    /**
     * @brief Apply PAX overrides on top of a parsed USTAR record and produce a populated
     * SharedData::DirectoryEntry to hand out to the caller.
     */
    SharedData::DirectoryEntry
    assembleDirectoryEntry(ParsedUstar const& ustar, PaxOverrides const& overrides);
}

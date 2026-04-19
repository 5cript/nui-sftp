#include <tar_archive/header.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ranges>
#include <string_view>

namespace TarArchive
{
    namespace
    {
        struct FieldLayout
        {
            std::size_t offset;
            std::size_t length;
        };

        constexpr FieldLayout fieldName{0u, 100u};
        constexpr FieldLayout fieldMode{100u, 8u};
        constexpr FieldLayout fieldUid{108u, 8u};
        constexpr FieldLayout fieldGid{116u, 8u};
        constexpr FieldLayout fieldSize{124u, 12u};
        constexpr FieldLayout fieldMtime{136u, 12u};
        constexpr FieldLayout fieldChecksum{148u, 8u};
        constexpr FieldLayout fieldTypeFlag{156u, 1u};
        constexpr FieldLayout fieldLinkName{157u, 100u};
        constexpr FieldLayout fieldMagic{257u, 6u};
        constexpr FieldLayout fieldVersion{263u, 2u};
        constexpr FieldLayout fieldUName{265u, 32u};
        constexpr FieldLayout fieldGName{297u, 32u};
        constexpr FieldLayout fieldDevMajor{329u, 8u};
        constexpr FieldLayout fieldDevMinor{337u, 8u};
        constexpr FieldLayout fieldPrefix{345u, 155u};

        constexpr std::string_view ustarMagic = "ustar";
        constexpr std::array<char, 2> ustarVersion{'0', '0'};

        std::span<std::byte> fieldSpan(RawRecord& record, FieldLayout field) noexcept
        {
            return std::span<std::byte>{record.data() + field.offset, field.length};
        }

        std::span<std::byte const> fieldSpan(RawRecord const& record, FieldLayout field) noexcept
        {
            return std::span<std::byte const>{record.data() + field.offset, field.length};
        }

        void writeString(RawRecord& record, FieldLayout field, std::string_view value) noexcept
        {
            const auto target = fieldSpan(record, field);
            const auto copyLength = std::min(value.size(), target.size());
            for (std::size_t position = 0u; position < copyLength; ++position)
                target[position] = static_cast<std::byte>(value[position]);
            for (std::size_t position = copyLength; position < target.size(); ++position)
                target[position] = std::byte{0u};
        }

        /**
         * @brief Write a number as a null-terminated octal ASCII string, left-padded with '0'.
         *
         * Returns false when the value does not fit; caller decides whether to fall back to
         * GNU base-256 encoding.
         */
        bool writeOctalNumeric(std::span<std::byte> target, std::uint64_t value) noexcept
        {
            if (target.empty())
                return false;

            std::array<char, 32u> scratch{};
            const auto conversion =
                std::to_chars(scratch.data(), scratch.data() + scratch.size(), value, 8);
            if (conversion.ec != std::errc{})
                return false;

            const std::size_t produced = static_cast<std::size_t>(conversion.ptr - scratch.data());
            const std::size_t digitCapacity = target.size() - 1u;
            if (produced > digitCapacity)
                return false;

            const std::size_t padding = digitCapacity - produced;
            for (std::size_t position = 0u; position < padding; ++position)
                target[position] = static_cast<std::byte>('0');
            for (std::size_t position = 0u; position < produced; ++position)
                target[padding + position] = static_cast<std::byte>(scratch[position]);
            target[digitCapacity] = std::byte{0u};
            return true;
        }

        /**
         * @brief Encode a 64-bit unsigned value as GNU base-256: high bit of the first byte
         * is set, remaining bytes hold a big-endian representation.
         */
        void writeBase256Numeric(std::span<std::byte> target, std::uint64_t value) noexcept
        {
            for (std::size_t position = target.size(); position > 0u; --position)
            {
                target[position - 1u] = static_cast<std::byte>(value & 0xFFu);
                value >>= 8u;
            }
            target[0] = static_cast<std::byte>(static_cast<std::uint8_t>(target[0]) | 0x80u);
        }

        /**
         * @brief Write a numeric field; tries octal first, falls back to base-256 if the
         * value would overflow the field's octal capacity.
         */
        void writeNumeric(RawRecord& record, FieldLayout field, std::uint64_t value) noexcept
        {
            const auto target = fieldSpan(record, field);
            if (!writeOctalNumeric(target, value))
                writeBase256Numeric(target, value);
        }

        /**
         * @brief Write the checksum field in its canonical ustar form: six octal digits,
         * a NUL byte, then a space.
         */
        void writeChecksum(RawRecord& record, std::uint32_t checksum) noexcept
        {
            const auto target = fieldSpan(record, fieldChecksum);
            std::array<char, 8u> scratch{};
            const auto conversion = std::to_chars(scratch.data(), scratch.data() + 6u, checksum, 8);
            const std::size_t produced = static_cast<std::size_t>(conversion.ptr - scratch.data());
            const std::size_t padding = 6u - produced;
            for (std::size_t position = 0u; position < padding; ++position)
                target[position] = static_cast<std::byte>('0');
            for (std::size_t position = 0u; position < produced; ++position)
                target[padding + position] = static_cast<std::byte>(scratch[position]);
            target[6u] = std::byte{0u};
            target[7u] = static_cast<std::byte>(' ');
        }

        std::string readTerminatedString(std::span<std::byte const> source) noexcept
        {
            std::string result;
            result.reserve(source.size());
            for (auto const& datum : source)
            {
                const char character = static_cast<char>(datum);
                if (character == '\0')
                    break;
                result.push_back(character);
            }
            return result;
        }

        /**
         * @brief Decode a numeric field. Handles both traditional octal and the GNU
         * base-256 extension (high bit of first byte set).
         */
        std::expected<std::uint64_t, TarError>
        readNumeric(std::span<std::byte const> source) noexcept
        {
            if (source.empty())
                return std::uint64_t{0u};

            const auto firstByte = static_cast<std::uint8_t>(source[0]);
            if ((firstByte & 0x80u) != 0u)
            {
                std::uint64_t value = firstByte & 0x7Fu;
                for (std::size_t position = 1u; position < source.size(); ++position)
                    value = (value << 8u) | static_cast<std::uint8_t>(source[position]);
                return value;
            }

            std::string digits;
            digits.reserve(source.size());
            for (auto const& datum : source)
            {
                const char character = static_cast<char>(datum);
                if (character == '\0' || character == ' ')
                    continue;
                if (character < '0' || character > '7')
                    return std::unexpected(makeError(
                        TarErrorCode::InvalidHeader,
                        std::string{"non-octal digit in numeric field: '"} + character + "'"
                    ));
                digits.push_back(character);
            }
            if (digits.empty())
                return std::uint64_t{0u};

            std::uint64_t value = 0u;
            const auto conversion =
                std::from_chars(digits.data(), digits.data() + digits.size(), value, 8);
            if (conversion.ec != std::errc{})
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "could not parse octal numeric field")
                );
            return value;
        }

        /**
         * @brief Split @p path into a USTAR (prefix, name) pair per the standard rules.
         *
         * Returns std::nullopt when no valid split exists — caller must then fall back to
         * PAX extended headers.
         */
        std::optional<std::pair<std::string, std::string>>
        trySplitUstarPath(std::string const& path, bool markDirectory)
        {
            const std::string nameSuffix = markDirectory ? "/" : "";
            if (path.size() + nameSuffix.size() <= fieldName.length)
                return std::pair<std::string, std::string>{std::string{}, path + nameSuffix};

            if (path.size() > fieldPrefix.length + 1u + fieldName.length)
                return std::nullopt;

            std::size_t splitIndex =
                path.size() > fieldName.length ? path.size() - fieldName.length : 0u;
            while (splitIndex < path.size() && path[splitIndex] != '/')
                ++splitIndex;

            for (; splitIndex < path.size(); ++splitIndex)
            {
                if (path[splitIndex] != '/')
                    continue;
                const std::string prefix = path.substr(0u, splitIndex);
                const std::string name = path.substr(splitIndex + 1u) + nameSuffix;
                if (prefix.empty() || prefix.size() > fieldPrefix.length)
                    continue;
                if (name.size() > fieldName.length)
                    continue;
                return std::pair<std::string, std::string>{prefix, name};
            }
            return std::nullopt;
        }

        std::string normaliseArchivePath(std::filesystem::path const& source)
        {
            std::string value = source.generic_string();
            while (!value.empty() && value.front() == '/')
                value.erase(value.begin());
            std::string collapsed;
            collapsed.reserve(value.size());
            bool previousWasSlash = false;
            for (char character : value)
            {
                if (character == '/' && previousWasSlash)
                    continue;
                collapsed.push_back(character);
                previousWasSlash = (character == '/');
            }
            return collapsed;
        }

        /**
         * @brief Construct the body of a PAX extended-header record ("len key=value\n"
         * items, where len counts itself).
         */
        std::string buildPaxPayload(std::span<std::pair<std::string, std::string> const> entries)
        {
            std::string payload;
            for (auto const& [key, value] : entries)
            {
                const std::string suffix = " " + key + "=" + value + "\n";
                std::size_t length = suffix.size() + 1u;
                std::string lengthDigits = std::to_string(length);
                while (lengthDigits.size() + suffix.size() != length)
                {
                    ++length;
                    lengthDigits = std::to_string(length);
                }
                payload += lengthDigits + suffix;
            }
            return payload;
        }

        void populateUstarHeader(
            RawRecord& record,
            std::string const& name,
            std::string const& prefix,
            SharedData::DirectoryEntry const& meta,
            TypeFlag typeflag,
            std::uint64_t payloadSize,
            std::string const& linkTarget
        )
        {
            record.fill(std::byte{0u});
            writeString(record, fieldName, name);
            writeString(record, fieldPrefix, prefix);
            writeNumeric(record, fieldMode, static_cast<std::uint32_t>(meta.permissions) & 07777u);
            writeNumeric(record, fieldUid, meta.uid);
            writeNumeric(record, fieldGid, meta.gid);
            writeNumeric(record, fieldSize, payloadSize);
            writeNumeric(record, fieldMtime, meta.mtime);
            record[fieldTypeFlag.offset] = static_cast<std::byte>(static_cast<char>(typeflag));
            writeString(record, fieldLinkName, linkTarget);
            writeString(record, fieldMagic, ustarMagic);
            fieldSpan(record, fieldVersion)[0] = static_cast<std::byte>(ustarVersion[0]);
            fieldSpan(record, fieldVersion)[1] = static_cast<std::byte>(ustarVersion[1]);
            writeString(record, fieldUName, meta.owner);
            writeString(record, fieldGName, meta.group);
            writeNumeric(record, fieldDevMajor, 0u);
            writeNumeric(record, fieldDevMinor, 0u);

            for (std::size_t position = 0u; position < fieldChecksum.length; ++position)
                record[fieldChecksum.offset + position] = static_cast<std::byte>(' ');
            const std::uint32_t checksum = calculateChecksum(record);
            writeChecksum(record, checksum);
        }

        template <typename IntegerType>
        std::expected<IntegerType, TarError> parseDecimal(std::string const& digits) noexcept
        {
            IntegerType value = 0;
            const auto conversion =
                std::from_chars(digits.data(), digits.data() + digits.size(), value, 10);
            if (conversion.ec != std::errc{})
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "malformed decimal value: " + digits)
                );
            return value;
        }

        struct PaxRecordSlice
        {
            std::string key;
            std::string value;
            std::size_t recordLength;
        };

        /**
         * @brief Extract one "len key=value\n" record from @p payload starting at @p cursor.
         * Populates the returned slice and the consumed byte count.
         */
        std::expected<PaxRecordSlice, TarError>
        extractPaxRecord(std::span<std::byte const> payload, std::size_t cursor) noexcept
        {
            const auto findByte = [&](std::size_t start, char target) noexcept -> std::size_t {
                for (std::size_t position = start; position < payload.size(); ++position)
                    if (static_cast<char>(payload[position]) == target)
                        return position;
                return payload.size();
            };

            const auto sliceToString = [&](std::size_t start, std::size_t stop) {
                std::string result;
                result.reserve(stop - start);
                for (std::size_t position = start; position < stop; ++position)
                    result.push_back(static_cast<char>(payload[position]));
                return result;
            };

            const std::size_t spacePosition = findByte(cursor, ' ');
            if (spacePosition == payload.size())
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "PAX record missing length delimiter")
                );

            const auto lengthParsed = parseDecimal<std::size_t>(sliceToString(cursor, spacePosition));
            if (!lengthParsed)
                return std::unexpected(lengthParsed.error());
            const std::size_t recordLength = *lengthParsed;

            if (recordLength == 0u || cursor + recordLength > payload.size())
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "PAX record length out of range")
                );

            const std::size_t recordEnd = cursor + recordLength;
            if (payload[recordEnd - 1u] != static_cast<std::byte>('\n'))
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "PAX record missing trailing newline")
                );

            const std::size_t keyStart = spacePosition + 1u;
            const std::size_t equalsPosition = findByte(keyStart, '=');
            if (equalsPosition >= recordEnd - 1u)
                return std::unexpected(makeError(TarErrorCode::InvalidHeader, "PAX record missing '='"));

            return PaxRecordSlice{
                sliceToString(keyStart, equalsPosition),
                sliceToString(equalsPosition + 1u, recordEnd - 1u),
                recordLength
            };
        }

        /**
         * @brief Apply one parsed PAX record to the accumulator. Unknown keys are ignored.
         */
        std::expected<void, TarError>
        applyPaxRecord(PaxRecordSlice const& slice, PaxOverrides& target)
        {
            if (slice.key == "path")
            {
                target.path = slice.value;
            }
            else if (slice.key == "linkpath")
            {
                target.linkPath = slice.value;
            }
            else if (slice.key == "size")
            {
                const auto parsed = parseDecimal<std::uint64_t>(slice.value);
                if (!parsed)
                    return std::unexpected(parsed.error());
                target.size = *parsed;
            }
            else if (slice.key == "mtime")
            {
                double seconds = 0.0;
                try
                {
                    seconds = std::stod(slice.value);
                }
                catch (...)
                {
                    return std::unexpected(makeError(TarErrorCode::InvalidHeader, "malformed PAX mtime"));
                }
                target.mtime = static_cast<std::uint64_t>(seconds);
            }
            else if (slice.key == "uid")
            {
                const auto parsed = parseDecimal<std::uint32_t>(slice.value);
                if (!parsed)
                    return std::unexpected(parsed.error());
                target.uid = *parsed;
            }
            else if (slice.key == "gid")
            {
                const auto parsed = parseDecimal<std::uint32_t>(slice.value);
                if (!parsed)
                    return std::unexpected(parsed.error());
                target.gid = *parsed;
            }
            else if (slice.key == "uname")
            {
                target.uName = slice.value;
            }
            else if (slice.key == "gname")
            {
                target.gName = slice.value;
            }
            return {};
        }
    }

    std::expected<TypeFlag, TarError> typeFlagFromFileType(SharedData::FileType type) noexcept
    {
        using enum SharedData::FileType;
        switch (type)
        {
            case Regular: return TypeFlag::RegularFile;
            case Directory: return TypeFlag::Directory;
            case Symlink: return TypeFlag::SymbolicLink;
            case CharDevice: return TypeFlag::CharacterSpecial;
            case BlockDevice: return TypeFlag::BlockSpecial;
            case Fifo: return TypeFlag::FifoSpecial;
            case Socket:
            case Special:
            case Unknown:
                return std::unexpected(
                    makeError(TarErrorCode::UnsupportedFileType, "file type has no USTAR representation")
                );
        }
        return std::unexpected(makeError(TarErrorCode::UnsupportedFileType, "unknown FileType value"));
    }

    SharedData::FileType fileTypeFromTypeFlag(TypeFlag flag) noexcept
    {
        using enum SharedData::FileType;
        switch (flag)
        {
            case TypeFlag::RegularFile:
            case TypeFlag::RegularFileAlternate:
            case TypeFlag::ContiguousFile:
            case TypeFlag::HardLink:
                return Regular;
            case TypeFlag::Directory: return Directory;
            case TypeFlag::SymbolicLink: return Symlink;
            case TypeFlag::CharacterSpecial: return CharDevice;
            case TypeFlag::BlockSpecial: return BlockDevice;
            case TypeFlag::FifoSpecial: return Fifo;
            case TypeFlag::ExtendedHeader:
            case TypeFlag::GlobalExtendedHeader:
                return Unknown;
        }
        return Unknown;
    }

    std::uint32_t calculateChecksum(RawRecord const& record) noexcept
    {
        std::uint32_t sum = 0u;
        for (std::size_t position = 0u; position < record.size(); ++position)
        {
            const bool insideChecksumField =
                position >= fieldChecksum.offset &&
                position < fieldChecksum.offset + fieldChecksum.length;
            if (insideChecksumField)
                sum += static_cast<std::uint32_t>(' ');
            else
                sum += static_cast<std::uint32_t>(static_cast<std::uint8_t>(record[position]));
        }
        return sum;
    }

    bool isZeroRecord(RawRecord const& record) noexcept
    {
        return std::ranges::all_of(record, [](std::byte datum) noexcept {
            return datum == std::byte{0u};
        });
    }

    std::string ParsedUstar::fullName() const
    {
        if (prefix.empty())
            return name;
        return prefix + "/" + name;
    }

    std::expected<ParsedUstar, TarError> parseRecord(RawRecord const& record)
    {
        const auto checksumStored = readNumeric(fieldSpan(record, fieldChecksum));
        if (!checksumStored)
            return std::unexpected(checksumStored.error());
        const std::uint32_t checksumComputed = calculateChecksum(record);
        if (static_cast<std::uint64_t>(checksumComputed) != *checksumStored)
            return std::unexpected(
                makeError(TarErrorCode::ChecksumMismatch, "USTAR record checksum does not match")
            );

        const auto modeValue = readNumeric(fieldSpan(record, fieldMode));
        if (!modeValue)
            return std::unexpected(modeValue.error());

        const auto uidValue = readNumeric(fieldSpan(record, fieldUid));
        if (!uidValue)
            return std::unexpected(uidValue.error());

        const auto gidValue = readNumeric(fieldSpan(record, fieldGid));
        if (!gidValue)
            return std::unexpected(gidValue.error());

        const auto sizeValue = readNumeric(fieldSpan(record, fieldSize));
        if (!sizeValue)
            return std::unexpected(sizeValue.error());

        const auto mtimeValue = readNumeric(fieldSpan(record, fieldMtime));
        if (!mtimeValue)
            return std::unexpected(mtimeValue.error());

        const std::string magic = readTerminatedString(fieldSpan(record, fieldMagic));

        ParsedUstar parsed;
        parsed.name = readTerminatedString(fieldSpan(record, fieldName));
        parsed.prefix = readTerminatedString(fieldSpan(record, fieldPrefix));
        parsed.permissions = static_cast<std::filesystem::perms>(*modeValue & 07777u);
        parsed.uid = static_cast<std::uint32_t>(*uidValue);
        parsed.gid = static_cast<std::uint32_t>(*gidValue);
        parsed.size = *sizeValue;
        parsed.mtime = *mtimeValue;
        parsed.typeflag = static_cast<TypeFlag>(static_cast<char>(record[fieldTypeFlag.offset]));
        parsed.linkName = readTerminatedString(fieldSpan(record, fieldLinkName));
        parsed.hasUstarMagic = (magic == std::string{ustarMagic});
        parsed.uName = readTerminatedString(fieldSpan(record, fieldUName));
        parsed.gName = readTerminatedString(fieldSpan(record, fieldGName));
        return parsed;
    }

    std::expected<PaxOverrides, TarError> parsePaxPayload(std::span<std::byte const> payload)
    {
        PaxOverrides overrides;
        std::size_t cursor = 0u;
        while (cursor < payload.size())
        {
            auto slice = extractPaxRecord(payload, cursor);
            if (!slice)
                return std::unexpected(slice.error());
            const auto applied = applyPaxRecord(*slice, overrides);
            if (!applied)
                return std::unexpected(applied.error());
            cursor += slice->recordLength;
        }
        return overrides;
    }

    SharedData::DirectoryEntry
    assembleDirectoryEntry(ParsedUstar const& ustar, PaxOverrides const& overrides)
    {
        SharedData::DirectoryEntry entry{};

        std::string chosenPath = overrides.path.value_or(ustar.fullName());
        if (ustar.typeflag == TypeFlag::Directory && !chosenPath.empty() &&
            chosenPath.back() == '/')
            chosenPath.pop_back();
        entry.path = std::filesystem::path{chosenPath};
        entry.fullPath = entry.path;
        entry.type = fileTypeFromTypeFlag(ustar.typeflag);
        entry.size = overrides.size.value_or(ustar.size);
        entry.uid = overrides.uid.value_or(ustar.uid);
        entry.gid = overrides.gid.value_or(ustar.gid);
        entry.owner = overrides.uName.value_or(ustar.uName);
        entry.group = overrides.gName.value_or(ustar.gName);
        entry.permissions = ustar.permissions;
        entry.mtime = overrides.mtime.value_or(ustar.mtime);

        if (ustar.typeflag == TypeFlag::SymbolicLink)
        {
            const std::string linkText = overrides.linkPath.value_or(ustar.linkName);
            entry.linkTarget = std::filesystem::path{linkText};
        }
        return entry;
    }

    std::expected<BuiltEntry, TarError> buildRecords(SharedData::DirectoryEntry const& meta)
    {
        const auto typeflagOrError = typeFlagFromFileType(meta.type);
        if (!typeflagOrError)
            return std::unexpected(typeflagOrError.error());
        const TypeFlag typeflag = *typeflagOrError;

        const bool isDirectory = (typeflag == TypeFlag::Directory);
        const std::string normalisedPath =
            normaliseArchivePath(meta.fullPath.empty() ? meta.path : meta.fullPath);
        if (normalisedPath.empty())
            return std::unexpected(makeError(TarErrorCode::InvalidHeader, "entry path is empty"));

        std::string linkTargetText;
        if (typeflag == TypeFlag::SymbolicLink)
        {
            if (!meta.linkTarget)
                return std::unexpected(
                    makeError(TarErrorCode::InvalidHeader, "symlink entry has no linkTarget set")
                );
            linkTargetText = meta.linkTarget->generic_string();
        }

        const std::uint64_t payloadSize = isDirectory ? 0u : meta.size;
        const bool sizeExceedsOctal = payloadSize > 077777777777ull;

        std::vector<std::pair<std::string, std::string>> paxEntries;
        std::string truncatedPath = normalisedPath;
        std::string truncatedLink = linkTargetText;

        auto split = trySplitUstarPath(normalisedPath, isDirectory);
        const bool pathNeedsPax = !split.has_value();
        if (pathNeedsPax)
        {
            paxEntries.emplace_back("path", normalisedPath);
            truncatedPath = normalisedPath.substr(0u, fieldName.length);
            split = std::pair<std::string, std::string>{std::string{}, truncatedPath};
        }

        const bool linkNeedsPax = linkTargetText.size() > fieldLinkName.length;
        if (linkNeedsPax)
        {
            paxEntries.emplace_back("linkpath", linkTargetText);
            truncatedLink = linkTargetText.substr(0u, fieldLinkName.length);
        }

        if (sizeExceedsOctal)
            paxEntries.emplace_back("size", std::to_string(payloadSize));

        BuiltEntry built;

        if (!paxEntries.empty())
        {
            const std::string paxPayload = buildPaxPayload(paxEntries);
            SharedData::DirectoryEntry paxMeta{};
            paxMeta.permissions = std::filesystem::perms{0644u};
            paxMeta.mtime = meta.mtime;
            paxMeta.owner = meta.owner;
            paxMeta.group = meta.group;
            paxMeta.uid = meta.uid;
            paxMeta.gid = meta.gid;
            paxMeta.size = paxPayload.size();

            const std::string paxName =
                "PaxHeaders/" + (truncatedPath.empty() ? std::string{"entry"} : truncatedPath);
            const auto paxSplit = trySplitUstarPath(paxName, false);
            const std::string paxNameField = paxSplit
                ? paxSplit->second
                : paxName.substr(0u, fieldName.length);
            const std::string paxPrefixField = paxSplit ? paxSplit->first : std::string{};

            RawRecord paxRecord;
            populateUstarHeader(
                paxRecord, paxNameField, paxPrefixField, paxMeta,
                TypeFlag::ExtendedHeader, paxPayload.size(), std::string{}
            );
            built.records.push_back(paxRecord);

            const std::size_t paxRecordCount = (paxPayload.size() + recordSize - 1u) / recordSize;
            for (std::size_t recordIndex = 0u; recordIndex < paxRecordCount; ++recordIndex)
            {
                RawRecord payloadRecord{};
                const std::size_t sliceOffset = recordIndex * recordSize;
                const std::size_t sliceLength = std::min(recordSize, paxPayload.size() - sliceOffset);
                for (std::size_t position = 0u; position < sliceLength; ++position)
                    payloadRecord[position] = static_cast<std::byte>(paxPayload[sliceOffset + position]);
                built.records.push_back(payloadRecord);
            }
        }

        const std::uint64_t recordedSize = sizeExceedsOctal ? 0u : payloadSize;
        RawRecord mainRecord;
        populateUstarHeader(
            mainRecord, split->second, split->first, meta, typeflag, recordedSize, truncatedLink
        );
        built.records.push_back(mainRecord);
        built.payloadSize = payloadSize;
        return built;
    }
}

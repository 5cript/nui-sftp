#pragma once

#include <command-store/types.hpp>
#include <utility/expected.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The JSON format snippets are copied and pasted in; shared by frontend and backend.
 *
 * {"version": 1, "snippets": [{"name", "command", "folder", "tags", "favorite"}]}, where folder is
 * a folder name. A bare array of entries or a single entry object is accepted as well.
 */
namespace CommandStore::SnippetTransfer
{
    constexpr std::int64_t currentVersion = 1;

    enum class ParseError
    {
        InvalidJson,
        UnsupportedDocument,
        UnsupportedVersion,
        EntryWithoutName,
        EntryWithoutCommand,
    };

    /**
     * @brief Why a document was rejected.
     */
    struct ParseFailure
    {
        ParseError error{ParseError::InvalidJson};

        /** @brief Zero based index of the offending entry, for the entry errors. */
        std::size_t entryIndex{0};

        /** @brief The rejected version for UnsupportedVersion. */
        std::string detail{};
    };

    using ParseResult = Utility::Expected<std::vector<TransferSnippet>, ParseFailure>;

    namespace Detail
    {
        inline std::string trimmed(std::string_view text)
        {
            constexpr std::string_view whitespace = " \t\r\n";
            const auto first = text.find_first_not_of(whitespace);
            if (first == std::string_view::npos)
                return {};
            const auto last = text.find_last_not_of(whitespace);
            return std::string{text.substr(first, last - first + 1)};
        }

        inline std::string stringOr(nlohmann::json const& entry, char const* key)
        {
            const auto value = entry.find(key);
            if (value == entry.end() || !value->is_string())
                return {};
            return value->get<std::string>();
        }

        inline Utility::Expected<TransferSnippet, ParseFailure>
        entryFromJson(nlohmann::json const& entry, std::size_t index)
        {
            if (!entry.is_object())
                return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument, .entryIndex = index}};

            TransferSnippet snippet{
                .name = trimmed(stringOr(entry, "name")),
                .command = stringOr(entry, "command"),
                .folder = trimmed(stringOr(entry, "folder")),
            };
            if (snippet.name.empty())
                return Utility::Unexpected{ParseFailure{.error = ParseError::EntryWithoutName, .entryIndex = index}};
            if (snippet.command.empty())
                return Utility::Unexpected{ParseFailure{.error = ParseError::EntryWithoutCommand, .entryIndex = index}};

            if (const auto tags = entry.find("tags"); tags != entry.end() && tags->is_array())
            {
                for (auto const& tag : *tags)
                {
                    if (tag.is_string())
                        snippet.tags.push_back(tag.get<std::string>());
                }
            }
            if (const auto favorite = entry.find("favorite"); favorite != entry.end() && favorite->is_boolean())
                snippet.favorite = favorite->get<bool>();
            return snippet;
        }

        inline ParseResult entriesFromJson(nlohmann::json const& entries)
        {
            std::vector<TransferSnippet> snippets{};
            snippets.reserve(entries.size());
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                auto snippet = entryFromJson(entries[index], index);
                if (!snippet)
                    return Utility::Unexpected{std::move(snippet.error())};
                snippets.push_back(std::move(*snippet));
            }
            return snippets;
        }
    }

    /**
     * @brief Reads snippets from an already parsed document; all or nothing.
     */
    inline ParseResult fromJson(nlohmann::json const& document)
    {
        if (document.is_array())
            return Detail::entriesFromJson(document);
        if (!document.is_object())
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};

        const auto snippets = document.find("snippets");
        if (snippets == document.end())
        {
            if (document.contains("name") || document.contains("command"))
                return Detail::entryFromJson(document, 0)
                    .transform(
                        [](TransferSnippet snippet)
                        {
                            return std::vector<TransferSnippet>{std::move(snippet)};
                        }
                    );
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};
        }

        if (const auto version = document.find("version"); version != document.end())
        {
            if (!version->is_number_integer() || version->get<std::int64_t>() > currentVersion)
                return Utility::Unexpected{
                    ParseFailure{.error = ParseError::UnsupportedVersion, .detail = version->dump()}
                };
        }
        if (!snippets->is_array())
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};
        return Detail::entriesFromJson(*snippets);
    }

    /**
     * @brief Parses pasted text; all or nothing.
     */
    inline ParseResult parse(std::string_view text)
    {
        const auto document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (document.is_discarded())
            return Utility::Unexpected{ParseFailure{.error = ParseError::InvalidJson}};
        return fromJson(document);
    }

    /**
     * @brief Builds the versioned document, keys in a stable order for readable output.
     */
    inline nlohmann::ordered_json toJson(std::vector<TransferSnippet> const& snippets)
    {
        auto entries = nlohmann::ordered_json::array();
        for (auto const& snippet : snippets)
        {
            auto entry = nlohmann::ordered_json::object();
            entry["name"] = snippet.name;
            entry["command"] = snippet.command;
            entry["folder"] = snippet.folder;
            entry["tags"] = snippet.tags;
            entry["favorite"] = snippet.favorite;
            entries.push_back(std::move(entry));
        }
        auto document = nlohmann::ordered_json::object();
        document["version"] = currentVersion;
        document["snippets"] = std::move(entries);
        return document;
    }

    /**
     * @brief Serializes import entries for the importSnippets RPC.
     */
    inline nlohmann::json importEntriesToJson(std::vector<ImportEntry> const& entries)
    {
        auto array = nlohmann::json::array();
        for (auto const& entry : entries)
        {
            array.push_back({
                {"name", entry.snippet.name},
                {"command", entry.snippet.command},
                {"folder", entry.snippet.folder},
                {"tags", entry.snippet.tags},
                {"favorite", entry.snippet.favorite},
                {"folderId", entry.folderId},
                {"replaces", entry.replaces},
            });
        }
        return array;
    }

    /**
     * @brief Reads import entries of the importSnippets RPC; all or nothing.
     */
    inline Utility::Expected<std::vector<ImportEntry>, ParseFailure> importEntriesFromJson(nlohmann::json const& array)
    {
        if (!array.is_array())
            return Utility::Unexpected{ParseFailure{.error = ParseError::UnsupportedDocument}};

        std::vector<ImportEntry> entries{};
        entries.reserve(array.size());
        for (std::size_t index = 0; index < array.size(); ++index)
        {
            auto snippet = Detail::entryFromJson(array[index], index);
            if (!snippet)
                return Utility::Unexpected{std::move(snippet.error())};
            entries.push_back(
                ImportEntry{
                    .snippet = std::move(*snippet),
                    .folderId = Detail::stringOr(array[index], "folderId"),
                    .replaces = Detail::stringOr(array[index], "replaces"),
                }
            );
        }
        return entries;
    }
}

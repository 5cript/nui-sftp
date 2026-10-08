#pragma once

#include <utility/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CommandStore
{
    /**
     * @brief Sort order for history listings.
     */
    enum class SortOrder
    {
        Recent,
        MostRun,
        Name,
    };

    /**
     * @brief One recorded command execution, deduplicated on (host, command).
     */
    struct HistoryEntry
    {
        /** @brief Database row id. */
        std::int64_t id{0};

        /** @brief Host label the command ran on, e.g. "user@host" or a local shell path. */
        std::string host{};

        /** @brief The command line, raw and binary-safe. */
        std::string command{};

        /** @brief Unix epoch seconds of the first recorded run. */
        std::int64_t firstRun{0};

        /** @brief Unix epoch seconds of the most recent run. */
        std::int64_t lastRun{0};

        /** @brief How often this (host, command) pair was executed. */
        std::int64_t runs{0};

        /** @brief Pinned entries are exempt from history cap trimming. */
        bool pinned{false};

        /** @brief User-marked favorite. */
        bool favorite{false};
    };

    /**
     * @brief Filter and ordering options for listing history.
     */
    struct HistoryQuery
    {
        /** @brief Only return entries for this host when set. */
        std::optional<std::string> hostFilter{};

        /** @brief Ordering of the result. */
        SortOrder sort{SortOrder::Recent};

        /** @brief Maximum number of returned entries when set. */
        std::optional<std::int64_t> limit{};
    };

    /**
     * @brief A folder grouping snippets.
     */
    struct SnippetFolder
    {
        /** @brief Folder id; empty on upsert means "generate one". */
        std::string id{};

        /** @brief Display name. */
        std::string name{};

        /** @brief Icon name. */
        std::string icon{};

        /** @brief Manual sort position. */
        std::int64_t position{0};
    };

    /**
     * @brief A saved, reusable command.
     */
    struct Snippet
    {
        /** @brief Snippet id; empty on upsert means "generate one". */
        std::string id{};

        /** @brief Display name. */
        std::string name{};

        /** @brief The command template, may contain {{variables}}. Raw and binary-safe. */
        std::string command{};

        /** @brief Id of the containing SnippetFolder; empty means root. */
        std::string folder{};

        /** @brief Free-form tags. */
        std::vector<std::string> tags{};

        /** @brief User-marked favorite. */
        bool favorite{false};

        /** @brief How often this snippet was run. */
        std::int64_t uses{0};

        /** @brief Unix epoch seconds of the last run; 0 if never run. */
        std::int64_t lastUsed{0};

        bool operator==(Snippet const&) const = default;
    };

    /**
     * @brief A snippet as exported and imported, with its folder by name instead of id.
     */
    struct TransferSnippet
    {
        /** @brief Display name. */
        std::string name{};

        /** @brief The command template, may contain {{variables}}. */
        std::string command{};

        /** @brief Name of the containing folder; empty means root. */
        std::string folder{};

        /** @brief Free-form tags. */
        std::vector<std::string> tags{};

        /** @brief User-marked favorite. */
        bool favorite{false};

        bool operator==(TransferSnippet const&) const = default;
    };

    /**
     * @brief One snippet to store in an import, with its target already decided.
     */
    struct ImportEntry
    {
        /** @brief The snippet; its folder name is only used when folderId is empty. */
        TransferSnippet snippet{};

        /**
         * @brief Id of the target folder. When empty, a non-empty snippet.folder names the folder,
         *        which is created when missing; otherwise the snippet goes to the root.
         */
        std::string folderId{};

        /** @brief Id of an existing snippet to overwrite; empty adds a new snippet. */
        std::string replaces{};

        bool operator==(ImportEntry const&) const = default;
    };

    /**
     * @brief Outcome of an import.
     */
    struct ImportSummary
    {
        /** @brief Snippets that were added. */
        std::int64_t added{0};

        /** @brief Existing snippets that were overwritten. */
        std::int64_t replaced{0};

        /** @brief Folders created because no folder of that name existed. */
        std::int64_t foldersCreated{0};
    };

    /**
     * @brief Error reported by store operations.
     */
    struct Error
    {
        /** @brief Human readable description. */
        std::string message{};

        /** @brief Extended sqlite error code; 0 when the error did not originate in sqlite. */
        int sqliteCode{0};
    };

    /**
     * @brief Result type of all store operations; sqlite errors are reported as values, not exceptions.
     */
    template <typename ValueT>
    using Result = Utility::Expected<ValueT, Error>;
}

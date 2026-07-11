#pragma once

#include <command-store/types.hpp>

#include <nui/event_system/observed_value.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Frontend facade of the backend command store; process global, shared by all sessions.
 *
 * Talks to the backend exclusively over the "CommandStore::*" RPCs and holds the observed lists the
 * history and snippet panels bind to. Mutations are applied optimistically to those lists and
 * reconciled with the backend reply, so panels react without waiting for a round trip.
 */
class CommandStoreClient
{
  public:
    CommandStoreClient();
    ROAR_PIMPL_SPECIAL_FUNCTIONS(CommandStoreClient);

    /**
     * @brief Sets the sink for failures that the user must see, e.g. a snippet that was not saved.
     *
     * Every failing operation reports here in addition to logging. Without a sink, failures are only
     * logged, which is not enough feedback for anything the user triggered.
     *
     * @param onError Receives a ready to display message.
     */
    void setOnError(std::function<void(std::string const&)> onError);

    /**
     * @brief The loaded history entries, in the order of the last reloadHistory query.
     */
    Nui::Observed<std::vector<CommandStore::HistoryEntry>>& history();

    /**
     * @brief All snippets, ordered by name.
     */
    Nui::Observed<std::vector<CommandStore::Snippet>>& snippets();

    /**
     * @brief All snippet folders, ordered by position.
     */
    Nui::Observed<std::vector<CommandStore::SnippetFolder>>& folders();

    /**
     * @brief The query the history was last loaded with; reused when a mutation triggers a reload.
     */
    CommandStore::HistoryQuery const& historyQuery() const;

    /**
     * @brief Records one execution and updates the local history optimistically.
     *
     * @param host Host label, e.g. "user@host" for ssh or the shell path for local sessions.
     */
    void recordExecution(std::string host, std::string command);

    /**
     * @brief Replaces the history list with the result of the query; the query is remembered.
     */
    void reloadHistory(CommandStore::HistoryQuery query = {}, std::function<void()> onLoaded = {});

    /**
     * @brief Replaces the snippet list.
     */
    void reloadSnippets(std::function<void()> onLoaded = {});

    /**
     * @brief Replaces the folder list.
     */
    void reloadFolders(std::function<void()> onLoaded = {});

    /**
     * @brief Sets pinned and/or favorite on one history entry; unset optionals leave the flag alone.
     */
    void setHistoryFlags(std::int64_t id, std::optional<bool> pinned, std::optional<bool> favorite);

    /**
     * @brief Deletes the given history entries.
     */
    void deleteHistory(std::vector<std::int64_t> ids);

    /**
     * @brief Deletes all history entries, including pinned ones.
     */
    void clearHistory();

    /**
     * @brief Inserts or updates a snippet; an empty id creates one.
     *
     * @param onSaved Receives the stored snippet, including its generated id.
     */
    void upsertSnippet(CommandStore::Snippet snippet, std::function<void(CommandStore::Snippet const&)> onSaved = {});

    /**
     * @brief Deletes a snippet.
     */
    void deleteSnippet(std::string id);

    /**
     * @brief Counts one run of a snippet and stamps its last use.
     */
    void bumpSnippetUse(std::string id);

    /**
     * @brief Inserts or updates a folder; an empty id creates one.
     *
     * @param onSaved Receives the stored folder, including its generated id.
     */
    void upsertFolder(
        CommandStore::SnippetFolder folder,
        std::function<void(CommandStore::SnippetFolder const&)> onSaved = {}
    );

    /**
     * @brief Deletes a folder; its snippets move to the root.
     */
    void deleteFolder(std::string id);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};

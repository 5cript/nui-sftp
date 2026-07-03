#pragma once

#include <command-store/types.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace CommandStore
{
    /**
     * @brief SQLite-backed store for command history and snippets.
     *
     * All operations are serialized on an internal asio strand, so the store is safe to use from
     * any thread without external synchronization. Every method returns immediately and invokes
     * its completion callback on the strand once the database work finished. Queued work keeps the
     * internal state alive, so destroying the store while operations are pending is safe; the
     * database closes after the last queued operation ran.
     *
     * No Nui / RPC dependencies; see StoreRpc for the RPC exposure.
     */
    class Store
    {
      public:
        constexpr static std::size_t defaultHistoryCap = 10'000;

        /**
         * @brief Opens (or creates) the database file and migrates the schema.
         *
         * @param executor Executor the internal strand is created on.
         * @param databaseFile Path to the sqlite database file, e.g. <programDirectory>/command_store.db.
         * @param historyCap Maximum number of history entries kept; oldest non-pinned entries are
         *                   trimmed beyond it.
         * @return The opened store, or the open/migration error.
         */
        static Result<Store> open(
            boost::asio::any_io_executor executor,
            std::filesystem::path const& databaseFile,
            std::size_t historyCap = defaultHistoryCap
        );

        ~Store();
        Store(Store const&) = delete;
        Store& operator=(Store const&) = delete;
        Store(Store&&) = default;
        Store& operator=(Store&&) = default;

        /**
         * @brief The strand all operations run on; share it (e.g. with RpcHelper::StrandRpc) to
         *        call back into the store without additional queueing.
         */
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand() const;

        /**
         * @brief Records one execution of command on host. Upserts on (host, command): a new pair
         *        starts with runs=1, an existing one increments runs and updates lastRun. Trims the
         *        oldest non-pinned entries beyond the history cap afterwards.
         *
         * @param nowEpoch Unix epoch seconds of the execution.
         * @param onComplete Receives the stored entry (with id and current runs); may be empty, in
         *                   which case failures are logged.
         */
        void recordExecution(
            std::string host,
            std::string command,
            std::int64_t nowEpoch,
            std::function<void(Result<HistoryEntry>)> onComplete = {}
        );

        /**
         * @brief Lists history entries according to the query.
         */
        void listHistory(HistoryQuery query, std::function<void(Result<std::vector<HistoryEntry>>)> onComplete);

        /**
         * @brief Sets pinned and/or favorite on one entry; unset optionals leave the flag unchanged.
         *        Fails when no entry with the given id exists.
         */
        void setHistoryFlags(
            std::int64_t id,
            std::optional<bool> pinned,
            std::optional<bool> favorite,
            std::function<void(Result<void>)> onComplete = {}
        );

        /**
         * @brief Deletes the entries with the given ids; unknown ids are ignored.
         */
        void deleteHistory(std::vector<std::int64_t> ids, std::function<void(Result<void>)> onComplete = {});

        /**
         * @brief Deletes all history entries, including pinned ones.
         */
        void clearHistory(std::function<void(Result<void>)> onComplete = {});

        /**
         * @brief Lists all snippets, ordered by name (case insensitive).
         */
        void listSnippets(std::function<void(Result<std::vector<Snippet>>)> onComplete);

        /**
         * @brief Inserts or updates a snippet by id; an empty id generates a new one. Updates never
         *        touch uses/lastUsed (see bumpSnippetUse).
         *
         * @param onComplete Receives the stored snippet (with generated id if any); may be empty.
         */
        void upsertSnippet(Snippet snippet, std::function<void(Result<Snippet>)> onComplete = {});

        /**
         * @brief Deletes a snippet; deleting an unknown id is a no-op.
         */
        void deleteSnippet(std::string id, std::function<void(Result<void>)> onComplete = {});

        /**
         * @brief Increments a snippet's use counter and stamps lastUsed. Fails when no snippet with
         *        the given id exists.
         *
         * @param nowEpoch Unix epoch seconds of the run.
         */
        void bumpSnippetUse(std::string id, std::int64_t nowEpoch, std::function<void(Result<void>)> onComplete = {});

        /**
         * @brief Lists all snippet folders, ordered by position, then name.
         */
        void listFolders(std::function<void(Result<std::vector<SnippetFolder>>)> onComplete);

        /**
         * @brief Inserts or updates a folder by id; an empty id generates a new one.
         *
         * @param onComplete Receives the stored folder (with generated id if any); may be empty.
         */
        void upsertFolder(SnippetFolder folder, std::function<void(Result<SnippetFolder>)> onComplete = {});

        /**
         * @brief Deletes a folder and moves its snippets to the root; deleting an unknown id is a no-op.
         */
        void deleteFolder(std::string id, std::function<void(Result<void>)> onComplete = {});

      private:
        struct Implementation;

        explicit Store(std::shared_ptr<Implementation> implementation);

      private:
        std::shared_ptr<Implementation> impl_;
    };
}

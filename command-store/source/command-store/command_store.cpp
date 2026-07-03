#include <command-store/command_store.hpp>

#include "sqlite/error.hpp"
#include "sqlite/statement.hpp"
#include "sqlite/transaction.hpp"

#include <log/log.hpp>

#include <fmt/format.h>
#include <boost/asio/dispatch.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace CommandStore
{
    namespace
    {
        std::string generateId()
        {
            return boost::uuids::to_string(boost::uuids::random_generator{}());
        }

        std::string tagsToJson(std::vector<std::string> const& tags)
        {
            return nlohmann::json(tags).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::vector<std::string> tagsFromJson(std::string const& text)
        {
            const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_array())
                return {};
            return parsed | std::views::filter([](nlohmann::json const& tag) {
                return tag.is_string();
            }) | std::views::transform([](nlohmann::json const& tag) {
                return tag.get<std::string>();
            }) | std::ranges::to<std::vector>();
        }

        HistoryEntry readHistoryEntry(Sqlite::Statement const& statement)
        {
            return HistoryEntry{
                .id = statement.columnInt64(0),
                .host = statement.columnText(1),
                .command = statement.columnText(2),
                .firstRun = statement.columnInt64(3),
                .lastRun = statement.columnInt64(4),
                .runs = statement.columnInt64(5),
                .pinned = statement.columnBool(6),
                .favorite = statement.columnBool(7),
            };
        }

        Snippet readSnippet(Sqlite::Statement const& statement)
        {
            return Snippet{
                .id = statement.columnText(0),
                .name = statement.columnText(1),
                .command = statement.columnText(2),
                .folder = statement.columnText(3),
                .tags = tagsFromJson(statement.columnText(4)),
                .favorite = statement.columnBool(5),
                .uses = statement.columnInt64(6),
                .lastUsed = statement.columnInt64(7),
            };
        }

        SnippetFolder readFolder(Sqlite::Statement const& statement)
        {
            return SnippetFolder{
                .id = statement.columnText(0),
                .name = statement.columnText(1),
                .icon = statement.columnText(2),
                .position = statement.columnInt64(3),
            };
        }

        /**
         * @brief Steps a prepared query to exhaustion, reading one row per step.
         */
        template <typename ReaderT>
        auto fetchAll(Sqlite::Statement& statement, ReaderT&& reader)
            -> Result<std::vector<std::decay_t<decltype(reader(statement))>>>
        {
            std::vector<std::decay_t<decltype(reader(statement))>> rows{};
            while (true)
            {
                auto hasRow = statement.step();
                if (!hasRow)
                    return Sqlite::failure(std::move(hasRow.error()));
                if (!*hasRow)
                    return rows;
                rows.push_back(reader(statement));
            }
        }

        constexpr char const* schemaVersion1 = R"sql(
            CREATE TABLE IF NOT EXISTS history (
                id INTEGER PRIMARY KEY,
                host TEXT NOT NULL,
                command TEXT NOT NULL,
                first_run INTEGER NOT NULL,
                last_run INTEGER NOT NULL,
                runs INTEGER NOT NULL DEFAULT 1,
                pinned INTEGER NOT NULL DEFAULT 0,
                favorite INTEGER NOT NULL DEFAULT 0,
                UNIQUE(host, command)
            );
            CREATE INDEX IF NOT EXISTS history_last_run ON history(last_run);
            CREATE TABLE IF NOT EXISTS snippet_folders (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                icon TEXT NOT NULL DEFAULT '',
                position INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS snippets (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                command TEXT NOT NULL,
                folder TEXT NOT NULL DEFAULT '',
                tags TEXT NOT NULL DEFAULT '[]',
                favorite INTEGER NOT NULL DEFAULT 0,
                uses INTEGER NOT NULL DEFAULT 0,
                last_used INTEGER NOT NULL DEFAULT 0
            );
            PRAGMA user_version = 1;
        )sql";

        Result<void> migrate(sqlite3* database)
        {
            return Sqlite::Statement::prepare(database, "PRAGMA user_version")
                .and_then(
                    [](Sqlite::Statement query)
                    {
                        return query.step().transform(
                            [&query](bool hasRow)
                            {
                                return hasRow ? query.columnInt64(0) : std::int64_t{0};
                            }
                        );
                    }
                )
                .and_then(
                    [database](std::int64_t version) -> Result<void>
                    {
                        if (version >= 1)
                            return {};
                        return Sqlite::Transaction::begin(database)
                            .and_then(
                                [database](Sqlite::Transaction transaction)
                                {
                                    return Sqlite::execute(database, schemaVersion1)
                                        .and_then(
                                            [&transaction]()
                                            {
                                                return transaction.commit();
                                            }
                                        );
                                }
                            );
                    }
                );
        }

        /**
         * @brief Hands the result to the callback; failures without a callback are logged so they
         *        do not vanish.
         */
        template <typename ValueT>
        void complete(std::function<void(Result<ValueT>)> const& onComplete, Result<ValueT> result)
        {
            if (onComplete)
                return onComplete(std::move(result));
            if (!result)
                Log::error("CommandStore operation failed: {}", result.error().message);
        }
    }

    struct Store::Implementation
    {
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand;
        std::size_t historyCap;
        Sqlite::DatabaseHandle database;

        /**
         * @brief Takes ownership of an already opened and migrated connection; see Store::open.
         */
        Implementation(boost::asio::any_io_executor executor, std::size_t historyCap, Sqlite::DatabaseHandle database)
            : strand{std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(std::move(executor))}
            , historyCap{historyCap}
            , database{std::move(database)}
        {}

        Result<void> trimHistory()
        {
            return Sqlite::Statement::prepare(database.get(), "SELECT COUNT(*) FROM history")
                .and_then(
                    [](Sqlite::Statement count)
                    {
                        return count.step().transform(
                            [&count](bool hasRow)
                            {
                                return hasRow ? count.columnInt64(0) : std::int64_t{0};
                            }
                        );
                    }
                )
                .and_then(
                    [this](std::int64_t total) -> Result<void>
                    {
                        const auto excess = total - static_cast<std::int64_t>(historyCap);
                        if (excess <= 0)
                            return {};
                        return Sqlite::Statement::prepare(
                                   database.get(),
                                   "DELETE FROM history WHERE id IN "
                                   "(SELECT id FROM history WHERE pinned = 0 ORDER BY last_run ASC, id ASC LIMIT ?1)"
                        )
                            .and_then(
                                [excess](Sqlite::Statement trim)
                                {
                                    trim.bind(1, excess);
                                    return trim.step();
                                }
                            )
                            .transform([](bool) {});
                    }
                );
        }
    };

    Result<Store> Store::open(
        boost::asio::any_io_executor executor,
        std::filesystem::path const& databaseFile,
        std::size_t historyCap
    )
    {
        sqlite3* rawDatabase = nullptr;
        const int opened = sqlite3_open_v2(
            databaseFile.string().c_str(), &rawDatabase, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr
        );
        Sqlite::DatabaseHandle database{rawDatabase};
        if (opened != SQLITE_OK)
        {
            return Sqlite::failure(Error{
                fmt::format(
                    "Could not open command store database '{}': {}",
                    databaseFile.string(),
                    database ? sqlite3_errmsg(database.get()) : "out of memory"
                ),
                database ? sqlite3_extended_errcode(database.get()) : SQLITE_NOMEM,
            });
        }

        sqlite3_busy_timeout(database.get(), 5'000);
        return Sqlite::execute(database.get(), "PRAGMA journal_mode = WAL")
            .and_then(
                [&database]()
                {
                    return Sqlite::execute(database.get(), "PRAGMA synchronous = NORMAL");
                }
            )
            .and_then(
                [&database]()
                {
                    return migrate(database.get());
                }
            )
            .transform(
                [&]()
                {
                    return Store{std::make_shared<Implementation>(std::move(executor), historyCap, std::move(database))};
                }
            );
    }

    Store::Store(std::shared_ptr<Implementation> implementation)
        : impl_{std::move(implementation)}
    {}

    Store::~Store() = default;

    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> Store::strand() const
    {
        return impl_->strand;
    }

    void Store::recordExecution(
        std::string host,
        std::string command,
        std::int64_t nowEpoch,
        std::function<void(Result<HistoryEntry>)> onComplete
    )
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_,
                host = std::move(host),
                command = std::move(command),
                nowEpoch,
                onComplete = std::move(onComplete)]()
            {
                complete<HistoryEntry>(
                    onComplete,
                    Sqlite::Transaction::begin(impl->database.get())
                        .and_then(
                            [&](Sqlite::Transaction transaction)
                            {
                                return Sqlite::Statement::prepare(
                                           impl->database.get(),
                                           "INSERT INTO history(host, command, first_run, last_run, runs, pinned, "
                                           "favorite) "
                                           "VALUES(?1, ?2, ?3, ?3, 1, 0, 0) "
                                           "ON CONFLICT(host, command) DO UPDATE SET "
                                           "runs = runs + 1, last_run = excluded.last_run "
                                           "RETURNING id, host, command, first_run, last_run, runs, pinned, favorite"
                                )
                                    .and_then(
                                        [&](Sqlite::Statement upsert) -> Result<HistoryEntry>
                                        {
                                            upsert.bind(1, host).bind(2, command).bind(3, nowEpoch);
                                            return upsert.step().and_then(
                                                [&](bool hasRow) -> Result<HistoryEntry>
                                                {
                                                    if (!hasRow)
                                                        return Sqlite::failure("history upsert returned no row");
                                                    return readHistoryEntry(upsert);
                                                }
                                            );
                                        }
                                    )
                                    .and_then(
                                        [&](HistoryEntry entry)
                                        {
                                            return impl->trimHistory()
                                                .and_then(
                                                    [&]()
                                                    {
                                                        return transaction.commit();
                                                    }
                                                )
                                                .transform(
                                                    [&]()
                                                    {
                                                        return std::move(entry);
                                                    }
                                                );
                                        }
                                    );
                            }
                        )
                );
            }
        );
    }

    void Store::listHistory(HistoryQuery query, std::function<void(Result<std::vector<HistoryEntry>>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, query = std::move(query), onComplete = std::move(onComplete)]()
            {
                constexpr auto orderClause = [](SortOrder sort) -> std::string_view
                {
                    switch (sort)
                    {
                        case SortOrder::Recent:
                            return " ORDER BY last_run DESC, id DESC";
                        case SortOrder::MostRun:
                            return " ORDER BY runs DESC, last_run DESC, id DESC";
                        case SortOrder::Name:
                            return " ORDER BY command COLLATE NOCASE ASC, id ASC";
                    }
                    return " ORDER BY last_run DESC, id DESC";
                };
                const auto sql = fmt::format(
                    "SELECT id, host, command, first_run, last_run, runs, pinned, favorite FROM history{}{}{}",
                    query.hostFilter ? " WHERE host = ?" : "",
                    orderClause(query.sort),
                    query.limit ? " LIMIT ?" : ""
                );

                complete<std::vector<HistoryEntry>>(
                    onComplete,
                    Sqlite::Statement::prepare(impl->database.get(), sql)
                        .and_then(
                            [&](Sqlite::Statement list)
                            {
                                int bindIndex = 1;
                                if (query.hostFilter)
                                    list.bind(bindIndex++, *query.hostFilter);
                                if (query.limit)
                                    list.bind(bindIndex++, *query.limit);
                                return fetchAll(list, readHistoryEntry);
                            }
                        )
                );
            }
        );
    }

    void Store::setHistoryFlags(
        std::int64_t id,
        std::optional<bool> pinned,
        std::optional<bool> favorite,
        std::function<void(Result<void>)> onComplete
    )
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, id, pinned, favorite, onComplete = std::move(onComplete)]()
            {
                complete<void>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(),
                        "UPDATE history SET pinned = COALESCE(?1, pinned), favorite = COALESCE(?2, favorite) "
                        "WHERE id = ?3"
                    )
                        .and_then(
                            [&](Sqlite::Statement update)
                            {
                                update.bind(1, pinned).bind(2, favorite).bind(3, id);
                                return update.step();
                            }
                        )
                        .and_then(
                            [&](bool) -> Result<void>
                            {
                                if (sqlite3_changes(impl->database.get()) == 0)
                                    return Sqlite::failure(fmt::format("no history entry with id {}", id));
                                return {};
                            }
                        )
                );
            }
        );
    }

    void Store::deleteHistory(std::vector<std::int64_t> ids, std::function<void(Result<void>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, ids = std::move(ids), onComplete = std::move(onComplete)]()
            {
                complete<void>(
                    onComplete,
                    Sqlite::Transaction::begin(impl->database.get())
                        .and_then(
                            [&](Sqlite::Transaction transaction)
                            {
                                return Sqlite::Statement::prepare(
                                           impl->database.get(), "DELETE FROM history WHERE id = ?1"
                                )
                                    .and_then(
                                        [&](Sqlite::Statement remove) -> Result<void>
                                        {
                                            for (const auto id : ids)
                                            {
                                                remove.bind(1, id);
                                                auto stepped = remove.step();
                                                if (!stepped)
                                                    return Sqlite::failure(std::move(stepped.error()));
                                                remove.reset();
                                            }
                                            return {};
                                        }
                                    )
                                    .and_then(
                                        [&]()
                                        {
                                            return transaction.commit();
                                        }
                                    );
                            }
                        )
                );
            }
        );
    }

    void Store::clearHistory(std::function<void(Result<void>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, onComplete = std::move(onComplete)]()
            {
                complete<void>(onComplete, Sqlite::execute(impl->database.get(), "DELETE FROM history"));
            }
        );
    }

    void Store::listSnippets(std::function<void(Result<std::vector<Snippet>>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, onComplete = std::move(onComplete)]()
            {
                complete<std::vector<Snippet>>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(),
                        "SELECT id, name, command, folder, tags, favorite, uses, last_used FROM snippets "
                        "ORDER BY name COLLATE NOCASE ASC, id ASC"
                    )
                        .and_then(
                            [](Sqlite::Statement list)
                            {
                                return fetchAll(list, readSnippet);
                            }
                        )
                );
            }
        );
    }

    void Store::upsertSnippet(Snippet snippet, std::function<void(Result<Snippet>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, snippet = std::move(snippet), onComplete = std::move(onComplete)]() mutable
            {
                if (snippet.id.empty())
                    snippet.id = generateId();

                complete<Snippet>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(),
                        "INSERT INTO snippets(id, name, command, folder, tags, favorite, uses, last_used) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                        "ON CONFLICT(id) DO UPDATE SET "
                        "name = excluded.name, command = excluded.command, folder = excluded.folder, "
                        "tags = excluded.tags, favorite = excluded.favorite "
                        "RETURNING id, name, command, folder, tags, favorite, uses, last_used"
                    )
                        .and_then(
                            [&](Sqlite::Statement upsert) -> Result<Snippet>
                            {
                                upsert.bind(1, snippet.id)
                                    .bind(2, snippet.name)
                                    .bind(3, snippet.command)
                                    .bind(4, snippet.folder)
                                    .bind(5, tagsToJson(snippet.tags))
                                    .bind(6, static_cast<std::int64_t>(snippet.favorite ? 1 : 0))
                                    .bind(7, snippet.uses)
                                    .bind(8, snippet.lastUsed);
                                return upsert.step().and_then(
                                    [&](bool hasRow) -> Result<Snippet>
                                    {
                                        if (!hasRow)
                                            return Sqlite::failure("snippet upsert returned no row");
                                        return readSnippet(upsert);
                                    }
                                );
                            }
                        )
                );
            }
        );
    }

    void Store::deleteSnippet(std::string id, std::function<void(Result<void>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, id = std::move(id), onComplete = std::move(onComplete)]()
            {
                complete<void>(
                    onComplete,
                    Sqlite::Statement::prepare(impl->database.get(), "DELETE FROM snippets WHERE id = ?1")
                        .and_then(
                            [&](Sqlite::Statement remove)
                            {
                                remove.bind(1, id);
                                return remove.step();
                            }
                        )
                        .transform([](bool) {})
                );
            }
        );
    }

    void Store::bumpSnippetUse(std::string id, std::int64_t nowEpoch, std::function<void(Result<void>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, id = std::move(id), nowEpoch, onComplete = std::move(onComplete)]()
            {
                complete<void>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(), "UPDATE snippets SET uses = uses + 1, last_used = ?1 WHERE id = ?2"
                    )
                        .and_then(
                            [&](Sqlite::Statement update)
                            {
                                update.bind(1, nowEpoch).bind(2, id);
                                return update.step();
                            }
                        )
                        .and_then(
                            [&](bool) -> Result<void>
                            {
                                if (sqlite3_changes(impl->database.get()) == 0)
                                    return Sqlite::failure(fmt::format("no snippet with id {}", id));
                                return {};
                            }
                        )
                );
            }
        );
    }

    void Store::listFolders(std::function<void(Result<std::vector<SnippetFolder>>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, onComplete = std::move(onComplete)]()
            {
                complete<std::vector<SnippetFolder>>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(),
                        "SELECT id, name, icon, position FROM snippet_folders "
                        "ORDER BY position ASC, name COLLATE NOCASE ASC, id ASC"
                    )
                        .and_then(
                            [](Sqlite::Statement list)
                            {
                                return fetchAll(list, readFolder);
                            }
                        )
                );
            }
        );
    }

    void Store::upsertFolder(SnippetFolder folder, std::function<void(Result<SnippetFolder>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, folder = std::move(folder), onComplete = std::move(onComplete)]() mutable
            {
                if (folder.id.empty())
                    folder.id = generateId();

                complete<SnippetFolder>(
                    onComplete,
                    Sqlite::Statement::prepare(
                        impl->database.get(),
                        "INSERT INTO snippet_folders(id, name, icon, position) VALUES(?1, ?2, ?3, ?4) "
                        "ON CONFLICT(id) DO UPDATE SET "
                        "name = excluded.name, icon = excluded.icon, position = excluded.position "
                        "RETURNING id, name, icon, position"
                    )
                        .and_then(
                            [&](Sqlite::Statement upsert) -> Result<SnippetFolder>
                            {
                                upsert.bind(1, folder.id)
                                    .bind(2, folder.name)
                                    .bind(3, folder.icon)
                                    .bind(4, folder.position);
                                return upsert.step().and_then(
                                    [&](bool hasRow) -> Result<SnippetFolder>
                                    {
                                        if (!hasRow)
                                            return Sqlite::failure("folder upsert returned no row");
                                        return readFolder(upsert);
                                    }
                                );
                            }
                        )
                );
            }
        );
    }

    void Store::deleteFolder(std::string id, std::function<void(Result<void>)> onComplete)
    {
        boost::asio::dispatch(
            *impl_->strand,
            [impl = impl_, id = std::move(id), onComplete = std::move(onComplete)]()
            {
                complete<void>(
                    onComplete,
                    Sqlite::Transaction::begin(impl->database.get())
                        .and_then(
                            [&](Sqlite::Transaction transaction)
                            {
                                return Sqlite::Statement::prepare(
                                           impl->database.get(), "UPDATE snippets SET folder = '' WHERE folder = ?1"
                                )
                                    .and_then(
                                        [&](Sqlite::Statement orphan)
                                        {
                                            orphan.bind(1, id);
                                            return orphan.step();
                                        }
                                    )
                                    .and_then(
                                        [&](bool)
                                        {
                                            return Sqlite::Statement::prepare(
                                                       impl->database.get(),
                                                       "DELETE FROM snippet_folders WHERE id = ?1"
                                            )
                                                .and_then(
                                                    [&](Sqlite::Statement remove)
                                                    {
                                                        remove.bind(1, id);
                                                        return remove.step();
                                                    }
                                                );
                                        }
                                    )
                                    .and_then(
                                        [&](bool)
                                        {
                                            return transaction.commit();
                                        }
                                    );
                            }
                        )
                );
            }
        );
    }
}

#pragma once

#include <command-store/types.hpp>
#include <utility/expected.hpp>

#include <sqlite3.h>

#include <memory>
#include <string>
#include <string_view>

namespace CommandStore::Sqlite
{
    /**
     * @brief Deleter closing a sqlite connection.
     */
    struct DatabaseCloser
    {
        void operator()(sqlite3* database) const noexcept
        {
            sqlite3_close(database);
        }
    };

    /**
     * @brief Owning handle for a sqlite connection.
     */
    using DatabaseHandle = std::unique_ptr<sqlite3, DatabaseCloser>;

    /**
     * @brief Deleter finalizing a prepared statement.
     */
    struct StatementFinalizer
    {
        void operator()(sqlite3_stmt* statement) const noexcept
        {
            sqlite3_finalize(statement);
        }
    };

    /**
     * @brief Owning handle for a prepared statement.
     */
    using StatementHandle = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

    /**
     * @brief Deleter releasing sqlite-allocated memory.
     */
    struct MemoryFreer
    {
        void operator()(char* memory) const noexcept
        {
            sqlite3_free(memory);
        }
    };

    /**
     * @brief Owning handle for sqlite-allocated memory, e.g. error messages from sqlite3_exec.
     */
    using MemoryHandle = std::unique_ptr<char, MemoryFreer>;

    /**
     * @brief Builds an Error from the connection's current error state. Not noexcept: composing the
     *        message allocates.
     */
    Error makeError(sqlite3* database, std::string_view what);

    /**
     * @brief Shorthand for building the unexpected side of a Result.
     */
    Utility::Unexpected<Error> failure(Error error);

    /**
     * @brief Shorthand combining makeError and failure.
     */
    Utility::Unexpected<Error> failure(sqlite3* database, std::string_view what);

    /**
     * @brief Shorthand for a failure that did not originate in sqlite.
     */
    Utility::Unexpected<Error> failure(std::string message);

    /**
     * @brief Runs one or more result-less statements.
     */
    Result<void> execute(sqlite3* database, char const* sql);
}

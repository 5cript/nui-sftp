#pragma once

#include "error.hpp"

#include <command-store/types.hpp>

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace CommandStore::Sqlite
{
    /**
     * @brief RAII prepared statement with binary-safe binds and column reads. Bind failures are
     *        accumulated and reported by the next step(), so binds stay chainable.
     */
    class Statement
    {
      public:
        /**
         * @brief Prepares a statement on the given connection.
         */
        static Result<Statement> prepare(sqlite3* database, std::string_view sql);

        ~Statement() = default;
        Statement(Statement&&) = default;
        Statement& operator=(Statement&&) = default;
        Statement(Statement const&) = delete;
        Statement& operator=(Statement const&) = delete;

        /**
         * @brief Binds text, binary-safe (embedded NULs survive).
         */
        Statement& bind(int index, std::string const& value);

        Statement& bind(int index, std::int64_t value);

        /**
         * @brief Binds 0/1, or NULL when unset (pairs with COALESCE updates).
         */
        Statement& bind(int index, std::optional<bool> const& value);

        /**
         * @brief Steps the statement; true while a result row is available. Reports earlier bind
         *        failures as well.
         */
        Result<bool> step();

        /**
         * @brief Resets the statement and clears bindings and any accumulated bind failure.
         */
        void reset();

        /**
         * @brief Reads a text column, binary-safe (embedded NULs survive).
         */
        std::string columnText(int index) const;

        std::int64_t columnInt64(int index) const;

        bool columnBool(int index) const;

      private:
        Statement(sqlite3* database, StatementHandle statement);

      private:
        sqlite3* database_;
        StatementHandle statement_;
        std::optional<Error> bindError_{};
    };
}

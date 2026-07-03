#pragma once

#include "error.hpp"

#include <command-store/types.hpp>

#include <sqlite3.h>

namespace CommandStore::Sqlite
{
    /**
     * @brief Transaction guard; rolls back on destruction unless committed.
     */
    class Transaction
    {
      public:
        /**
         * @brief Begins an immediate transaction on the given connection.
         */
        static Result<Transaction> begin(sqlite3* database);

        ~Transaction();
        Transaction(Transaction&& other) noexcept;
        Transaction& operator=(Transaction&& other) noexcept;
        Transaction(Transaction const&) = delete;
        Transaction& operator=(Transaction const&) = delete;

        /**
         * @brief Commits; afterwards destruction is a no-op.
         */
        Result<void> commit();

      private:
        explicit Transaction(sqlite3* database);

        void rollback() noexcept;

      private:
        sqlite3* database_{nullptr};
    };
}

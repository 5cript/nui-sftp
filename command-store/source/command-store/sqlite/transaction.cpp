#include "transaction.hpp"

#include <utility>

namespace CommandStore::Sqlite
{
    Result<Transaction> Transaction::begin(sqlite3* database)
    {
        if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun)
            return failure(std::move(begun.error()));
        return Transaction{database};
    }

    Transaction::Transaction(sqlite3* database)
        : database_{database}
    {}

    Transaction::~Transaction()
    {
        rollback();
    }

    Transaction::Transaction(Transaction&& other) noexcept
        : database_{std::exchange(other.database_, nullptr)}
    {}

    Transaction& Transaction::operator=(Transaction&& other) noexcept
    {
        if (this != &other)
        {
            rollback();
            database_ = std::exchange(other.database_, nullptr);
        }
        return *this;
    }

    Result<void> Transaction::commit()
    {
        auto result = execute(database_, "COMMIT");
        if (result)
            database_ = nullptr;
        return result;
    }

    void Transaction::rollback() noexcept
    {
        if (database_)
        {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
            database_ = nullptr;
        }
    }
}

#include "statement.hpp"

#include <utility>

namespace CommandStore::Sqlite
{
    Result<Statement> Statement::prepare(sqlite3* database, std::string_view sql)
    {
        sqlite3_stmt* rawStatement = nullptr;
        const int prepared =
            sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &rawStatement, nullptr);
        StatementHandle statement{rawStatement};
        if (prepared != SQLITE_OK)
            return failure(database, "failed to prepare statement");
        return Statement{database, std::move(statement)};
    }

    Statement::Statement(sqlite3* database, StatementHandle statement)
        : database_{database}
        , statement_{std::move(statement)}
    {}

    Statement& Statement::bind(int index, std::string const& value)
    {
        if (!bindError_ &&
            sqlite3_bind_text(
                statement_.get(), index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT
            ) != SQLITE_OK)
            bindError_ = makeError(database_, "failed to bind text parameter");
        return *this;
    }

    Statement& Statement::bind(int index, std::int64_t value)
    {
        if (!bindError_ && sqlite3_bind_int64(statement_.get(), index, value) != SQLITE_OK)
            bindError_ = makeError(database_, "failed to bind integer parameter");
        return *this;
    }

    Statement& Statement::bind(int index, std::optional<bool> const& value)
    {
        if (!value)
        {
            if (!bindError_ && sqlite3_bind_null(statement_.get(), index) != SQLITE_OK)
                bindError_ = makeError(database_, "failed to bind null parameter");
            return *this;
        }
        return bind(index, static_cast<std::int64_t>(*value ? 1 : 0));
    }

    Result<bool> Statement::step()
    {
        if (bindError_)
            return failure(*bindError_);
        const int result = sqlite3_step(statement_.get());
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        return failure(database_, "failed to step statement");
    }

    void Statement::reset()
    {
        sqlite3_reset(statement_.get());
        sqlite3_clear_bindings(statement_.get());
        bindError_.reset();
    }

    std::string Statement::columnText(int index) const
    {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement_.get(), index));
        const int size = sqlite3_column_bytes(statement_.get(), index);
        if (!text)
            return {};
        return std::string{text, static_cast<std::size_t>(size)};
    }

    std::int64_t Statement::columnInt64(int index) const
    {
        return sqlite3_column_int64(statement_.get(), index);
    }

    bool Statement::columnBool(int index) const
    {
        return sqlite3_column_int64(statement_.get(), index) != 0;
    }
}

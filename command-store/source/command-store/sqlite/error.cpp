#include "error.hpp"

#include <fmt/format.h>

#include <utility>

namespace CommandStore::Sqlite
{
    Error makeError(sqlite3* database, std::string_view what)
    {
        return Error{
            fmt::format("{}: {}", what, sqlite3_errmsg(database)),
            sqlite3_extended_errcode(database),
        };
    }

    Utility::Unexpected<Error> failure(Error error)
    {
        return Utility::Unexpected<Error>{std::move(error)};
    }

    Utility::Unexpected<Error> failure(sqlite3* database, std::string_view what)
    {
        return failure(makeError(database, what));
    }

    Utility::Unexpected<Error> failure(std::string message)
    {
        return failure(Error{std::move(message), 0});
    }

    Result<void> execute(sqlite3* database, char const* sql)
    {
        char* rawErrorMessage = nullptr;
        const int result = sqlite3_exec(database, sql, nullptr, nullptr, &rawErrorMessage);
        const MemoryHandle errorMessage{rawErrorMessage};
        if (result != SQLITE_OK)
        {
            return failure(Error{
                errorMessage ? errorMessage.get() : "unknown sqlite error",
                sqlite3_extended_errcode(database),
            });
        }
        return {};
    }
}

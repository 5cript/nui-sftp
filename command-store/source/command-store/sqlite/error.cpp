#include "error.hpp"

#include <fmt/format.h>

#include <utility>

namespace CommandStore::Sqlite
{
    namespace
    {
        std::string withReadOnlyHint(sqlite3* database, std::string message, int extendedCode)
        {
            if (!isReadOnly(extendedCode))
                return message;
            return fmt::format("{}. {}", message, readOnlyHint(database));
        }
    }

    Error makeError(sqlite3* database, std::string_view what)
    {
        const auto code = sqlite3_extended_errcode(database);
        return Error{
            withReadOnlyHint(database, fmt::format("{}: {}", what, sqlite3_errmsg(database)), code),
            code,
        };
    }

    bool isReadOnly(int extendedCode)
    {
        return (extendedCode & 0xff) == SQLITE_READONLY;
    }

    std::string readOnlyHint(sqlite3* database)
    {
        char const* file = sqlite3_db_filename(database, "main");
        return fmt::format(
            "Make sure '{0}', '{0}-wal' and '{0}-shm' are writable, then restart the application",
            file != nullptr ? file : ""
        );
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
            const auto code = sqlite3_extended_errcode(database);
            return failure(Error{
                withReadOnlyHint(database, errorMessage ? errorMessage.get() : "unknown sqlite error", code),
                code,
            });
        }
        return {};
    }
}

#pragma once

#include <shared_data/file_operations/local_file_problem.hpp>

#include <system_error>

/**
 * @brief What @p error says about a local file: Locked when another program holds it, PermissionDenied, NotFound,
 *        NoSpace, or Other for anything else and for no error at all. Takes the system's codes of every platform,
 *        a Windows code in std::system_category() on Windows, and errno values in either category.
 */
SharedData::LocalFileProblem localFileProblemOf(std::error_code const& error);

namespace LocalFileProblemDetail
{
    /**
     * @brief What the Windows system error @p code says about a local file, see localFileProblemOf. Available on every
     *        platform, so the table can be checked anywhere.
     */
    SharedData::LocalFileProblem windowsFileProblemOf(int code);
}

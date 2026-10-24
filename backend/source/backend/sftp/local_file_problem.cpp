#include <backend/sftp/local_file_problem.hpp>

namespace
{
    using SharedData::LocalFileProblem;

    namespace WindowsError
    {
        constexpr int fileNotFound = 2;
        constexpr int pathNotFound = 3;
        constexpr int accessDenied = 5;
        constexpr int sharingViolation = 32;
        constexpr int lockViolation = 33;
        constexpr int diskFull = 112;
    }

    LocalFileProblem portableFileProblemOf(std::error_code const& error)
    {
        if (error == std::errc::device_or_resource_busy || error == std::errc::text_file_busy)
            return LocalFileProblem::Locked;
        if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
            return LocalFileProblem::PermissionDenied;
        if (error == std::errc::no_such_file_or_directory)
            return LocalFileProblem::NotFound;
        if (error == std::errc::no_space_on_device)
            return LocalFileProblem::NoSpace;
        return LocalFileProblem::Other;
    }
}

namespace LocalFileProblemDetail
{
    SharedData::LocalFileProblem windowsFileProblemOf(int code)
    {
        switch (code)
        {
            case WindowsError::sharingViolation:
            case WindowsError::lockViolation:
                return LocalFileProblem::Locked;
            case WindowsError::accessDenied:
                return LocalFileProblem::PermissionDenied;
            case WindowsError::fileNotFound:
            case WindowsError::pathNotFound:
                return LocalFileProblem::NotFound;
            case WindowsError::diskFull:
                return LocalFileProblem::NoSpace;
            default:
                return LocalFileProblem::Other;
        }
    }
}

SharedData::LocalFileProblem localFileProblemOf(std::error_code const& error)
{
#ifdef _WIN32
    // Its values are Windows codes, some of which an errno comparison would take for unrelated errno values.
    if (error.category() == std::system_category())
        return LocalFileProblemDetail::windowsFileProblemOf(error.value());
#endif
    return portableFileProblemOf(error);
}

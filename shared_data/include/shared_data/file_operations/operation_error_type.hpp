#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <utility/enum_string_convert.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(
        OperationErrorType,
        ImplementationError, // This should never occur, it indicates a bug in the program:
        UnknownError, // This should also never occur, indicates improper error forwarding.
        NotImplemented,
        FileExists,
        FileNotFound,
        FileSeekFailure,
        OpenFailure,
        FileStreamExpired,
        FileStatFailed,
        SftpError,
        InvalidPath,
        RenameFailure,
        CannotSetFilePermissions,
        FutureTimeout,
        OperationNotPrepared,
        CannotFinalizeDuringRead,
        InvalidOptionsKey,
        TargetFileNotGood,
        SourceFileNotGood,
        CannotWorkCompletedOperation,
        CannotWorkFailedOperation,
        CannotWorkCanceledOperation,
        CannotCreateDirectory,
        UnknownWorkState,
        InvalidOperationState,
        OperationNotPossibleOnFileType,
        FilesystemError,
        DeleteFailed,
        CannotCreateSymlink,
        SymlinkPointsToDirectory
    );
}
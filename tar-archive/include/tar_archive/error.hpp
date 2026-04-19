#pragma once

#include <utility/describe.hpp>

#include <fmt/format.h>

#include <string>

namespace TarArchive
{
    /**
     * @brief Enumerated error codes produced by the tar-archive library.
     *
     * Grouped roughly into: environmental (FileNotFound…), format (MagicMismatch…),
     * API-misuse (EntryStillOpen…), and third-party backend (CompressionLibraryError).
     */
    enum class TarErrorCode
    {
        None = 0,

        FileNotFound,
        FileAlreadyExists,
        IoError,

        UnknownCompression,
        MagicMismatch,

        PathTooLong,
        LinkTargetTooLong,
        UnsupportedFileType,
        FileTooLarge,

        InvalidHeader,
        TruncatedArchive,
        ChecksumMismatch,

        EntryStillOpen,
        NoActiveEntry,
        AlreadyFinalized,
        AlreadyClosed,
        OverrunOnWrite,
        UnderrunOnClose,
        ReadAfterEnd,
        WriteAfterFinalize,

        CompressionLibraryError
    };

    BOOST_DESCRIBE_ENUM(
        TarErrorCode,
        None,
        FileNotFound,
        FileAlreadyExists,
        IoError,
        UnknownCompression,
        MagicMismatch,
        PathTooLong,
        LinkTargetTooLong,
        UnsupportedFileType,
        FileTooLarge,
        InvalidHeader,
        TruncatedArchive,
        ChecksumMismatch,
        EntryStillOpen,
        NoActiveEntry,
        AlreadyFinalized,
        AlreadyClosed,
        OverrunOnWrite,
        UnderrunOnClose,
        ReadAfterEnd,
        WriteAfterFinalize,
        CompressionLibraryError
    )

    /**
     * @brief Structured error value carried through std::expected return types.
     *
     * nativeCode is the underlying errno / zlib / bz2 / zstd / lzma return code when relevant,
     * otherwise zero. message is always populated with a short human description.
     */
    struct TarError
    {
        TarErrorCode code{TarErrorCode::None};
        std::string message{};
        int nativeCode{0};

        std::string toString() const
        {
            char const* name = boost::describe::enum_to_string(code, "INVALID_TAR_ERROR_CODE");
            if (nativeCode != 0)
                return fmt::format("{} (#{}): {}", name, nativeCode, message);
            return fmt::format("{}: {}", name, message);
        }
    };

    /**
     * @brief Shorthand factory used throughout the implementation.
     */
    inline TarError makeError(TarErrorCode code, std::string message, int nativeCode = 0)
    {
        return TarError{code, std::move(message), nativeCode};
    }
}

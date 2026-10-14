#pragma once

#include <ssh/file_stream_interface.hpp>
#include <ssh/sftp_error.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>

namespace ResumeCheck
{
    /** @brief How many bytes before the resume point have to agree. */
    constexpr std::uint64_t comparedBytes = 64u * 1024u;

    /**
     * @brief Whether the bytes right before @p size agree in the local file and the remote stream, so a leftover
     *        partial file belongs to this source and can be continued. A partial file of another program or of an
     *        older version of the source fails this. The position of @p remote is unspecified afterwards.
     *
     * @return False when they differ or either side is shorter than @p size.
     */
    std::expected<bool, SecureShell::SftpError>
    tailsMatchInStrand(SecureShell::IFileStream& remote, std::filesystem::path const& local, std::uint64_t size);
}

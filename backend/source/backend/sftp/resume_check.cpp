#include <backend/sftp/resume_check.hpp>

#include <algorithm>
#include <fstream>
#include <string>

namespace ResumeCheck
{
    std::expected<bool, SecureShell::SftpError>
    tailsMatchInStrand(SecureShell::IFileStream& remote, std::filesystem::path const& local, std::uint64_t size)
    {
        const auto count = std::min(size, comparedBytes);
        const auto offset = size - count;

        std::string localTail(static_cast<std::size_t>(count), '\0');
        std::ifstream localFile{local, std::ios::binary};
        if (!localFile.seekg(static_cast<std::streamoff>(offset)) ||
            !localFile.read(localTail.data(), static_cast<std::streamsize>(count)))
            return false;

        if (auto seeked = remote.seekInStrand(static_cast<std::size_t>(offset)); !seeked)
            return std::unexpected(seeked.error());
        std::string remoteTail(static_cast<std::size_t>(count), '\0');
        std::size_t filled = 0;
        while (filled < remoteTail.size())
        {
            const auto read = remote.readSomeInStrand(remoteTail.data() + filled, remoteTail.size() - filled);
            if (!read)
                return std::unexpected(read.error());
            if (*read == 0)
                return false;
            filled += *read;
        }
        return localTail == remoteTail;
    }
}

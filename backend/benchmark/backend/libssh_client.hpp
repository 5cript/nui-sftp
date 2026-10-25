#pragma once

#include "client.hpp"
#include "connection.hpp"

#include <libssh/sftp.h>

namespace Benchmark
{
    /**
     * @brief Moves the files one after another with libssh's sftp_aio calls directly, requests of the server's limit
     *        kept in flight: what the connection the app uses can do without the app.
     */
    class LibsshClient : public Client
    {
      public:
        explicit LibsshClient(Connection& connection);

        std::string name() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;

      private:
        std::expected<void, std::string> transferInStrand(Transfer const& transfer);
        std::expected<void, std::string>
        downloadFile(std::string const& remotePath, std::filesystem::path const& localPath, std::uint64_t size);
        std::expected<void, std::string>
        uploadFile(std::filesystem::path const& localPath, std::string const& remotePath, std::uint64_t size);
        std::expected<void, std::string> createRemoteDirectories(std::string const& remotePath);

      private:
        Connection& connection_;
        sftp_session sftp_{nullptr};
        std::size_t readLength_{0};
        std::size_t writeLength_{0};
    };
}

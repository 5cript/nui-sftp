#pragma once

#include "client.hpp"
#include "operation_driver.hpp"

#include <backend/sftp/unsafe_fast_mode.hpp>
#include <remote_io/remote_file_system.hpp>

#include <memory>
#include <string>

namespace Benchmark
{
    enum class UnsafeFast
    {
        Off,
        On,
    };

    /**
     * @brief Transfers with the operations of the app, configured like OperationQueue does for its defaults: a single
     *        file as an UploadOperation or DownloadOperation, a directory as a bulk transfer that scans first. With
     *        @p unsafeFast in the unsafe fast mode, see UnsafeFastMode.
     */
    class ApplicationClient : public Client
    {
      public:
        ApplicationClient(
            std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem,
            std::string name,
            Protocol protocol,
            UnsafeFast unsafeFast
        );

        std::string name() const override;
        Protocol protocol() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;

      private:
        std::expected<void, std::string> download(Transfer const& transfer);
        std::expected<void, std::string> upload(Transfer const& transfer);

        UnsafeFastMode unsafeFastMode() const;

      private:
        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem_;
        std::string name_;
        Protocol protocol_;
        bool unsafeFast_;
    };
}

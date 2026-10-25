#pragma once

#include "client.hpp"
#include "connection.hpp"
#include "operation_driver.hpp"

#include <backend/sftp/unsafe_fast_mode.hpp>

namespace Benchmark
{
    /**
     * @brief Transfers with the operations of the app, configured like OperationQueue does for its defaults: a single
     *        file as an UploadOperation or DownloadOperation, a directory as a bulk transfer that scans first. With
     *        @p unsafeFast in the unsafe fast mode, see UnsafeFastMode.
     */
    class ApplicationClient : public Client
    {
      public:
        ApplicationClient(Connection& connection, bool unsafeFast);

        std::string name() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;

      private:
        std::expected<void, std::string> download(Transfer const& transfer);
        std::expected<void, std::string> upload(Transfer const& transfer);

        UnsafeFastMode unsafeFastMode() const;

      private:
        Connection& connection_;
        bool unsafeFast_;
    };
}

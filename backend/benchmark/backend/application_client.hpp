#pragma once

#include "client.hpp"
#include "connection.hpp"
#include "operation_driver.hpp"

namespace Benchmark
{
    /**
     * @brief Transfers with the operations of the app, configured like OperationQueue does for its defaults: a single
     *        file as an UploadOperation or DownloadOperation, a directory as a bulk transfer that scans first.
     */
    class ApplicationClient : public Client
    {
      public:
        explicit ApplicationClient(Connection& connection);

        std::string name() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;

      private:
        std::expected<void, std::string> download(Transfer const& transfer);
        std::expected<void, std::string> upload(Transfer const& transfer);

      private:
        Connection& connection_;
    };
}

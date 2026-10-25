#pragma once

#include "client.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace Benchmark
{
    /**
     * @brief The sftp program of the host in batch mode, the reference the app is compared with. Starting it and
     *        logging in is measured with an empty batch and subtracted.
     */
    class OpenSshClient : public Client
    {
      public:
        struct Options
        {
            std::string host{};
            std::string user{};
            std::filesystem::path key{};
            /** @brief Where the batch files go. */
            std::filesystem::path workDirectory{};
        };

        /**
         * @brief Measures the overhead of a session.
         *
         * @return Why the baseline cannot run, e.g. sftp is missing on the host.
         */
        static std::expected<std::unique_ptr<OpenSshClient>, std::string> make(Options options);

        std::string name() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;
        std::chrono::nanoseconds fixedOverhead() const override
        {
            return overhead_;
        }

      private:
        explicit OpenSshClient(Options options);
        std::expected<void, std::string> runBatch(std::string const& commands) const;

      private:
        Options options_;
        std::chrono::nanoseconds overhead_{};
    };
}

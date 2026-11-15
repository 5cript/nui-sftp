#pragma once

#include "client.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace Benchmark
{
    /**
     * @brief The curl program of the host, the FTP reference the app is compared with: one curl per transfer, the
     *        files of a directory with --parallel as many at once as the app's bulk transfers. Starting it and logging
     *        in is measured with a request for the size of a file and subtracted.
     */
    class CurlClient : public Client
    {
      public:
        struct Options
        {
            std::string host{};
            std::uint16_t port{21u};
            std::string user{};
            std::string password{};
            /** @brief Ftp, or Ftps for AUTH TLS without a check of the certificate. */
            Protocol protocol{Protocol::Ftp};
            /** @brief A file on the server, its size is what the overhead run asks for. */
            std::string probePath{};
            /** @brief Where the config files go. */
            std::filesystem::path workDirectory{};
        };

        /**
         * @brief Measures the overhead of a session.
         *
         * @return Why the baseline cannot run, e.g. curl is missing on the host.
         */
        static std::expected<std::unique_ptr<CurlClient>, std::string> make(Options options);

        /** @brief The name of the client for @p protocol, Ftp or Ftps. */
        static std::string nameFor(Protocol protocol);

        std::string name() const override;
        Protocol protocol() const override;
        std::expected<void, std::string> transfer(Transfer const& transfer) override;
        std::chrono::nanoseconds fixedOverhead() const override
        {
            return overhead_;
        }
        std::chrono::nanoseconds fixedProcessorOverhead() const override
        {
            return processorOverhead_;
        }

      private:
        explicit CurlClient(Options options);
        std::string urlOf(std::string const& remotePath) const;
        std::expected<void, std::string> run(std::string const& requests) const;

      private:
        Options options_;
        std::chrono::nanoseconds overhead_{};
        std::chrono::nanoseconds processorOverhead_{};
    };
}

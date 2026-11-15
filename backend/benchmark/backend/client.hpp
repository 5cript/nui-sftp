#pragma once

#include "workload.hpp"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace Benchmark
{
    enum class Protocol
    {
        Sftp,
        Ftp,
        /** @brief FTP with AUTH TLS, control and data connections protected. */
        Ftps,
    };

    inline std::string_view toString(Protocol protocol)
    {
        switch (protocol)
        {
            case Protocol::Sftp:
                return "SFTP";
            case Protocol::Ftp:
                return "FTP";
            case Protocol::Ftps:
                return "FTPS";
        }
        return "unknown";
    }

    /**
     * @brief Something that moves a workload between the host and the server.
     */
    class Client
    {
      public:
        Client() = default;
        virtual ~Client() = default;
        Client(Client const&) = delete;
        Client& operator=(Client const&) = delete;
        Client(Client&&) = delete;
        Client& operator=(Client&&) = delete;

        virtual std::string name() const = 0;

        /** @brief What the client speaks, its rows are compared with the baseline of that protocol. */
        virtual Protocol protocol() const = 0;

        /** @brief Whether the client can move @p workload at all. */
        virtual bool supports(Workload const&) const
        {
            return true;
        }

        /**
         * @brief Moves the files of @p transfer, its target does not exist yet.
         */
        virtual std::expected<void, std::string> transfer(Transfer const& transfer) = 0;

        /** @brief Time every transfer spends on something else, subtracted from the measurement. */
        virtual std::chrono::nanoseconds fixedOverhead() const
        {
            return {};
        }

        /** @brief Processor time every transfer spends on something else, subtracted like fixedOverhead(). */
        virtual std::chrono::nanoseconds fixedProcessorOverhead() const
        {
            return {};
        }
    };
}

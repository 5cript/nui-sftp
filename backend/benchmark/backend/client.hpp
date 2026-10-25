#pragma once

#include "workload.hpp"

#include <chrono>
#include <expected>
#include <string>

namespace Benchmark
{
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
    };
}

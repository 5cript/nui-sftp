#pragma once

#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol
{
    /**
     * @brief One line of a FEAT reply.
     */
    struct Feature
    {
        /** @brief The feature label in upper case, "MLST" or "REST". */
        std::string name{};
        /** @brief What follows the label, "type*;size*;" or "STREAM", as the server sent it. */
        std::string parameters{};
    };

    /**
     * @brief How the MLST feature lists a fact (RFC 3659 7.8).
     */
    enum class FactState
    {
        /** @brief The server knows the fact, OPTS MLST has to ask for it. */
        Offered,
        /** @brief Marked with '*', MLST and MLSD send it now. */
        Enabled,
    };

    /**
     * @brief What a server says it supports in its reply to FEAT (RFC 2389). Servers do not list everything they
     *        support, vsftpd leaves out AUTH TLS and UTF8, so a missing feature is no proof.
     */
    class Features
    {
      public:
        Features() = default;

        /**
         * @brief Reads the reply to FEAT: the lines of a 211 reply that start with a space are features, an error reply
         *        other than 421 means none.
         *
         * @return The error of a 421 or of a reply that is neither 211 nor an error.
         */
        static std::expected<Features, RemoteIo::RemoteError> fromReply(Reply const& reply);

        /**
         * @brief The server lists @p name, in any case.
         */
        bool has(std::string_view name) const;

        /**
         * @brief The server lists @p name with @p parameter among its parameters, which are separated by ';' or
         *        spaces. "AUTH TLS;SSL" has TLS, "REST STREAM" has STREAM. Both compare in any case. The facts of
         *        MLST carry a '*' when enabled, ask machineListFact() for them.
         */
        bool has(std::string_view name, std::string_view parameter) const;

        /**
         * @brief How MLST lists @p fact, compared in any case and without the '*' that marks an enabled fact.
         *
         * @return Nothing when FEAT lists no MLST or MLST lists no such fact.
         */
        std::optional<FactState> machineListFact(std::string_view fact) const;

        /**
         * @brief The parameters of the first line for @p name.
         */
        std::optional<std::string_view> parametersOf(std::string_view name) const;

        std::vector<Feature> const& all() const;

      private:
        std::vector<Feature> features_{};
    };
}

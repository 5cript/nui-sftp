#pragma once

#include <ftp/support/ftp_script.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief One thing that happened on the server, for assertions on order and content.
     */
    struct TranscriptEntry
    {
        enum class Kind
        {
            /** @brief A control connection was accepted, the detail is the address of the peer. */
            Connected,
            /** @brief The detail is the line without its CRLF. */
            Command,
            /** @brief The detail names the connection and the TLS version, and says so if it was resumed. */
            TlsEstablished,
            /** @brief The detail names the connection and why. */
            TlsFailed,
            /** @brief A data connection was accepted, the detail is the address of the peer. */
            DataConnected,
            /** @brief A data connection came from another address than the control connection. */
            DataRefused,
            /** @brief A data connection did not resume a session, and the plan required that. */
            DataNotResumed,
            /** @brief The detail is what the client uploaded. */
            DataUploaded,
            /** @brief The detail says how the data connection ended. */
            DataEnded,
            /** @brief The detail says how the control connection ended. */
            Disconnected,
        };

        /** @brief Which control connection, counted from 0 in the order they were accepted. */
        std::size_t connection{0};
        Kind kind{};
        std::string detail{};
    };

    struct ScriptedFtpServerOptions
    {
        /** @brief Every control connection starts with a TLS handshake. */
        bool implicitTls{false};
        /** @brief Binds [::1] instead of 127.0.0.1. */
        bool ipv6{false};
        /** @brief Presented instead of TestPki::server. */
        std::optional<CertifiedKey> certificate{};
    };

    /**
     * @brief A deliberately simple FTP server for tests, on a thread of its own. Each control connection runs the
     *        next Script added to it. Tests wait on checkpoints and on waitUntilFinished(), never on sleeps.
     */
    class ScriptedFtpServer
    {
      public:
        class Implementation;
        using Options = ScriptedFtpServerOptions;

        /**
         * @brief Binds a free port and starts the thread.
         *
         * @return The running server, or why it could not start.
         */
        static std::expected<std::unique_ptr<ScriptedFtpServer>, std::string> start(Options options = {});

        ~ScriptedFtpServer();
        ScriptedFtpServer(ScriptedFtpServer const&) = delete;
        ScriptedFtpServer& operator=(ScriptedFtpServer const&) = delete;
        ScriptedFtpServer(ScriptedFtpServer&&) = delete;
        ScriptedFtpServer& operator=(ScriptedFtpServer&&) = delete;

        std::string address() const;
        std::uint16_t port() const;

        /**
         * @brief The next control connection runs @p script. A connection without one fails.
         */
        void addConnection(Script script);

        /**
         * @brief Waits until a script reached Script::checkpoint(@p name) and takes that checkpoint.
         *
         * @return False when none was reached in time.
         */
        bool waitForCheckpoint(std::string const& name, std::chrono::milliseconds timeout = std::chrono::seconds{10});

        /**
         * @brief Waits until every added script ended, by its last step or by a failure.
         *
         * @return False when one did not end in time.
         */
        bool waitUntilFinished(std::chrono::milliseconds timeout = std::chrono::seconds{10});

        /**
         * @brief Waits until every accepted control connection and its data plans ended, which is when commands
         *        behind the end of a script have been seen.
         *
         * @return False when one is still open.
         */
        bool waitUntilDisconnected(std::chrono::milliseconds timeout = std::chrono::seconds{10});

        std::vector<TranscriptEntry> transcript() const;

        /**
         * @brief The command lines received on control connection @p connection, in order.
         */
        std::vector<std::string> commands(std::size_t connection = 0u) const;

        /**
         * @brief Everything that went against a script, each entry a readable diff.
         */
        std::vector<std::string> failures() const;

      private:
        explicit ScriptedFtpServer(std::unique_ptr<Implementation> implementation);

      private:
        std::unique_ptr<Implementation> impl_;
    };
}

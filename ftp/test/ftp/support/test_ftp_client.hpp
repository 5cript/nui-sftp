#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FileTransferProtocol::Test
{
    struct TlsVerification
    {
        /** @brief The PEM certificate to trust, nothing accepts any certificate. */
        std::optional<std::string> authority{};
        /** @brief Checked against the certificate when an authority is set. */
        std::string hostName{};
    };

    /**
     * @brief A blocking client with just enough FTP to exercise servers from a test. It knows nothing of the product
     *        code on purpose, a bug there cannot hide in the test setup.
     */
    class TestFtpClient
    {
      public:
        struct Reply
        {
            int code{0};
            /** @brief All lines of the reply without their CRLF, joined by a newline. */
            std::string text{};
        };

        /**
         * @param timeout What each single operation may take.
         */
        explicit TestFtpClient(std::chrono::milliseconds timeout = std::chrono::seconds{10});
        ~TestFtpClient();
        TestFtpClient(TestFtpClient const&) = delete;
        TestFtpClient& operator=(TestFtpClient const&) = delete;
        TestFtpClient(TestFtpClient&&) = delete;
        TestFtpClient& operator=(TestFtpClient&&) = delete;

        std::expected<void, std::string> connect(std::string const& host, std::uint16_t port);

        /**
         * @brief Turns the control connection to TLS, for AUTH TLS and for implicit TLS.
         */
        std::expected<void, std::string> startTls(TlsVerification const& verification = {});

        std::expected<void, std::string> send(std::string_view line);
        std::expected<Reply, std::string> readReply();

        /**
         * @brief Sends @p line and reads the reply to it.
         */
        std::expected<Reply, std::string> command(std::string_view line);

        /**
         * @brief Connects a data connection.
         *
         * @param secured Starts TLS on it, offering the session of the control connection.
         */
        std::expected<void, std::string> openData(std::string const& host, std::uint16_t port, bool secured = false);

        /**
         * @brief Reads the data connection to its end.
         */
        std::expected<std::string, std::string> readData();

        std::expected<void, std::string> writeData(std::string_view bytes);

        /**
         * @brief Ends the data connection, over TLS with close_notify.
         */
        void closeData();

        /**
         * @brief Ends the control connection without a QUIT.
         */
        void close();

        /**
         * @brief The port of a 227 or 229 reply.
         */
        static std::optional<std::uint16_t> passivePort(Reply const& reply);

      private:
        class Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}

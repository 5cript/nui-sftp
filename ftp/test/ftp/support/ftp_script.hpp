#pragma once

#include <ftp/support/test_pki.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief How the server takes part in TLS, on the control connection or on a data connection.
     */
    struct TlsBehaviour
    {
        /** @brief Plaintext sent before the handshake, e.g. a reply injected behind the 234. */
        std::string plaintextFirst{};
        /** @brief Sent instead of a handshake, see fatalAlert(). */
        std::optional<std::string> insteadOfHandshake{};
        /** @brief Ends a data connection that did not resume a session, like vsftpd. */
        bool requireResumption{false};
        /** @brief Answers an offered session with a full handshake. */
        bool refuseResumption{false};
        /** @brief Issues no session tickets, like pure-ftpd. */
        bool issueNoTickets{false};
        /** @brief Presents this certificate instead of the one of the server. */
        std::optional<CertifiedKey> certificate{};
        /** @brief Ends the connection without close_notify. */
        bool closeWithoutCloseNotify{false};
    };

    /**
     * @brief The bytes of a fatal handshake_failure alert.
     */
    std::string fatalAlert();

    /**
     * @brief What a data connection does once the control script began it. Without a closing action it closes at the
     *        end of the plan.
     */
    struct DataPlan
    {
        struct Send
        {
            std::string bytes{};
            /** @brief Written in pieces of this size, 0 sends everything at once. */
            std::size_t chunkSize{0};
            std::chrono::milliseconds interval{};
        };
        struct SendForever
        {};
        struct ExpectUpload
        {
            std::size_t limit{0};
        };
        struct Close
        {};
        struct Reset
        {};
        struct StallAfter
        {
            std::string bytes{};
        };
        using Action = std::variant<Send, SendForever, ExpectUpload, Close, Reset, StallAfter>;

        DataPlan& send(std::string bytes, std::size_t chunkSize = 0, std::chrono::milliseconds interval = {});
        /** @brief Sends until the client goes away or the control script calls stopData(). */
        DataPlan& sendForever();
        /** @brief Reads until the client closes, more than @p limit bytes fail the script. */
        DataPlan& expectUpload(std::size_t limit);
        DataPlan& close();
        DataPlan& reset();
        /** @brief Sends @p bytes, then neither sends nor closes until the client goes away. */
        DataPlan& stallAfter(std::string bytes);
        DataPlan& tls(TlsBehaviour behaviour = {});
        /** @brief Announces this address and port instead of the real ones. */
        DataPlan& announce(std::string address, std::uint16_t port);
        /** @brief Replaces the whole reply, where {port}, {high} and {low} stand for the real port. */
        DataPlan& announceAs(std::string reply);
        DataPlan& acceptFromAnyone();
        DataPlan& acceptOnlyFromControlPeer();

        std::vector<Action> actions{};
        std::optional<TlsBehaviour> tlsBehaviour{};
        std::optional<std::string> announcedAddress{};
        std::optional<std::uint16_t> announcedPort{};
        std::optional<std::string> announcedReply{};
        bool onlyControlPeer{true};
    };

    /**
     * @brief The servers whose FEAT reply featLike() gives.
     */
    enum class ServerLike
    {
        Vsftpd,
        Proftpd,
        PureFtpd,
        Iis,
    };

    /**
     * @brief The replies real servers give to an ABOR after they closed the data connection.
     */
    enum class AbortReplies
    {
        /** @brief 426, 225. */
        VsftpdLike,
        /** @brief 426, 226. */
        ProftpdPlainLike,
        /** @brief 226, 426. */
        ProftpdTlsLike,
        /** @brief 426, a multi-line 226. */
        PureFtpdLike,
        /** @brief 426, 451. */
        PureFtpdFailedLike,
        /** @brief 226 alone, the transfer had ended. */
        FinalOnly,
    };

    enum class ListingFormat
    {
        Unix,
        Dos,
        Mlsd,
    };

    struct ListingEntry
    {
        std::string name{};
        std::uint64_t size{0};
        bool directory{false};
    };

    /**
     * @brief The bytes of a listing in @p format, for DataPlan::send().
     */
    std::string listing(ListingFormat format, std::vector<ListingEntry> const& entries);

    /**
     * @brief What one control connection of the ScriptedFtpServer does, step by step. A script ends silently: the
     *        server keeps the connection open and any further command fails it.
     */
    struct Script
    {
        struct Expect
        {
            std::string verb{};
            std::optional<std::string> argumentPattern{};
        };
        struct Send
        {
            std::string bytes{};
        };
        struct Delay
        {
            std::chrono::milliseconds duration{};
        };
        struct Dribble
        {
            std::string bytes{};
            std::chrono::milliseconds interval{};
        };
        struct CloseControl
        {};
        struct ResetControl
        {};
        struct Stall
        {};
        struct ExpectDisconnect
        {};
        struct StartTls
        {
            TlsBehaviour behaviour{};
        };
        struct Checkpoint
        {
            std::string name{};
        };
        struct Passive
        {
            DataPlan plan{};
        };
        struct BeginData
        {};
        struct AwaitData
        {};
        struct StopData
        {};
        using Step = std::variant<
            Expect,
            Send,
            Delay,
            Dribble,
            CloseControl,
            ResetControl,
            Stall,
            ExpectDisconnect,
            StartTls,
            Checkpoint,
            Passive,
            BeginData,
            AwaitData,
            StopData>;

        /** @brief Waits for the next command, which must be @p verb with any argument. */
        Script& expect(std::string verb);
        /** @brief Waits for the next command, whose argument must match the regular expression as a whole. */
        Script& expect(std::string verb, std::string argumentPattern);
        /** @brief Sends @p text as a reply: every line ends in CRLF, also the last one. */
        Script& reply(std::string_view text);
        /** @brief Sends @p bytes as they are. */
        Script& replyRaw(std::string bytes);
        /** @brief Sends @p text without a command it could answer, the same bytes as reply(). */
        Script& unsolicited(std::string_view text);
        Script& delay(std::chrono::milliseconds duration);
        /** @brief Sends @p bytes one at a time. */
        Script& dribble(std::string bytes, std::chrono::milliseconds interval = {});
        Script& closeControl();
        /** @brief Ends the connection with a TCP reset. */
        Script& resetControl();
        /** @brief Never answers again, the script counts as finished. */
        Script& stall();
        /** @brief Waits for the client to end the connection, a command fails the script. */
        Script& expectDisconnect();
        Script& startTls(TlsBehaviour behaviour = {});
        /** @brief A sync point, see ScriptedFtpServer::waitForCheckpoint(). */
        Script& checkpoint(std::string name);
        /** @brief Listens for a data connection and answers the PASV or EPSV expected before this step. */
        Script& passive(DataPlan plan);
        /** @brief Starts the plan of the last passive() once the client connected. */
        Script& beginData();
        /** @brief Waits until that plan ended. */
        Script& awaitData();
        /** @brief Ends a sendForever() or a stallAfter() of that plan. */
        Script& stopData();

        /** @brief Greets, then accepts a USER and PASS login. */
        Script& login(std::string user = "user", std::string password = "password");
        /** @brief Answers a FEAT like @p server does. */
        Script& featLike(ServerLike server);
        /** @brief Answers an ABOR with @p replies. */
        Script& abortSequence(AbortReplies replies);

        std::vector<Step> steps{};
    };
}

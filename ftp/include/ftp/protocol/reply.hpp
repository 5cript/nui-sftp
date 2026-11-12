#pragma once

#include <remote_io/remote_error.hpp>

#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol
{
    /**
     * @brief One reply of the server, RFC 959 4.2.
     */
    struct Reply
    {
        /** @brief The code of the line that ended the reply, three digits from 100 to 599. */
        int code{0};
        /** @brief The lines as received without their line ends and Telnet negotiation, codes included. */
        std::vector<std::string> lines{};
        /** @brief Lines of a multi-line reply past the limits, skipped up to its last line. */
        std::size_t droppedLines{0};
        /** @brief The reply ended on a line with another code than its first, which some servers send. */
        bool endedOnOtherCode{false};

        /**
         * @brief The text for a person: the lines without the codes that frame them, joined by newlines, printable.
         */
        std::string text() const;
    };

    /**
     * @brief Bounds on what the server may send, so a broken or hostile one cannot exhaust memory.
     */
    struct ReplyLimits
    {
        /** @brief Bytes of one line, its line end included. A longer line breaks the protocol. */
        std::size_t lineLength{8u * 1024u};
        /** @brief Bytes of the lines kept of one reply. */
        std::size_t replySize{256u * 1024u};
        /** @brief Lines kept of one reply. */
        std::size_t lineCount{10'000u};
    };

    /**
     * @brief Cuts the bytes of a control connection into replies, in whatever pieces they arrive (RFC 1123 4.1.2.10).
     *
     * A line ends at LF, a CR before it is dropped. Telnet option negotiation is removed and IAC IAC becomes 0xFF, IAC
     * before any other byte is a 0xFF the server did not escape and stays with that byte. Blank lines
     * between replies are skipped. A multi-line reply ends on a line with its own code and a space. A line inside it
     * with another code and a space is held as a possible end, for servers that end on another code: the next line
     * makes it text, unless settle() accepts it first because nothing followed. Lines past the limits of a multi-line
     * reply are skipped up to its last line (RFC 1123 4.1.2.11).
     */
    class ReplyFramer
    {
      public:
        explicit ReplyFramer(ReplyLimits limits = {});

        /**
         * @brief Takes the next bytes received.
         *
         * @return A ProtocolViolation for a line that starts no reply or is too long. The framer takes nothing after
         *         that and returns the same error again.
         */
        std::expected<void, RemoteIo::RemoteError> feed(std::string_view bytes);

        /**
         * @brief Takes the oldest complete reply.
         */
        std::optional<Reply> next();

        /**
         * @brief A line with another code may have ended the open reply. The connection calls settle() when no byte
         *        followed for a moment.
         */
        bool holdsPossibleEnd() const;

        /**
         * @brief Ends the open reply on the line held as its possible end, when there is one and no byte of a
         *        following line arrived.
         */
        void settle();

        /**
         * @brief The connection ended: settles. A last line without its line end may be cut short and completes no
         *        reply.
         *
         * @return ConnectionLost when a reply was left open or a last line had no end, a ProtocolViolation for a last
         *         line that starts no reply.
         */
        std::expected<void, RemoteIo::RemoteError> finish();

        /**
         * @brief Bytes of a reply arrived that did not complete it.
         */
        bool inReply() const;

      private:
        enum class TelnetState
        {
            Data,
            Command,
            Option,
        };

        std::expected<void, RemoteIo::RemoteError> takeByte(unsigned char byte);
        std::expected<void, RemoteIo::RemoteError> keepInLine(unsigned char byte);
        std::expected<void, RemoteIo::RemoteError> takeLine(std::string line);
        void keepInOpenReply(std::string line);
        void completeOpenReply(std::string lastLine, int code);

      private:
        ReplyLimits limits_;
        TelnetState telnetState_{TelnetState::Data};
        std::string line_{};
        std::optional<Reply> open_{};
        std::size_t openSize_{0};
        std::optional<std::string> possibleEnd_{};
        std::deque<Reply> complete_{};
        std::optional<RemoteIo::RemoteError> failure_{};
    };

    /**
     * @brief The reply code at the start of @p line: three digits, the first from 1 to 5, followed by a space, a hyphen
     *        or the end of the line.
     */
    std::optional<int> replyCodeOf(std::string_view line);

    /**
     * @brief @p line without the reply code and the separator after it, the whole line when it starts with no code.
     */
    std::string_view textOfLine(std::string_view line);
}

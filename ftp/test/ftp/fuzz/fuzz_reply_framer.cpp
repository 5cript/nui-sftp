#include <ftp/protocol/reply.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace FileTransferProtocol;

    // Small, so the fuzzer reaches every limit.
    constexpr ReplyLimits limits{.lineLength = 128u, .replySize = 512u, .lineCount = 8u};

    struct Framed
    {
        std::vector<Reply> replies{};
        bool failed{false};
        bool finishFailed{false};
    };

    void check(bool condition)
    {
        if (!condition)
            std::abort();
    }

    void checkReply(Reply const& reply)
    {
        check(reply.code >= 100 && reply.code <= 599);
        check(!reply.lines.empty());
        check(replyCodeOf(reply.lines.front()).has_value());
        check(reply.lines.size() <= limits.lineCount + 1u);
        std::size_t kept = 0u;
        for (auto const& line : reply.lines)
        {
            check(line.size() < limits.lineLength);
            kept += line.size();
        }
        check(kept <= limits.replySize + limits.lineLength);
        static_cast<void>(reply.text());
    }

    Framed frame(std::string_view bytes, std::size_t pieceSize)
    {
        ReplyFramer framer{limits};
        Framed framed{};
        for (std::size_t position = 0u; position < bytes.size() && !framed.failed; position += pieceSize)
        {
            framed.failed = !framer.feed(bytes.substr(position, pieceSize));
            while (auto reply = framer.next())
                framed.replies.push_back(std::move(*reply));
        }
        if (!framed.failed)
        {
            framed.finishFailed = !framer.finish();
            while (auto reply = framer.next())
                framed.replies.push_back(std::move(*reply));
        }
        return framed;
    }

    bool sameFraming(Framed const& left, Framed const& right)
    {
        return left.failed == right.failed && left.finishFailed == right.finishFailed &&
            std::ranges::equal(
                left.replies,
                right.replies,
                [](Reply const& first, Reply const& second)
                {
                    return first.code == second.code && first.lines == second.lines &&
                        first.droppedLines == second.droppedLines && first.endedOnOtherCode == second.endedOnOtherCode;
                }
            );
    }
}

/**
 * @brief Frames the input whole, byte by byte and in pieces of a size it chooses: every reply stays within the limits
 *        and the read boundaries change nothing (RFC 1123 4.1.2.10).
 */
extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    const std::string_view bytes{reinterpret_cast<char const*>(data), size};
    const auto whole = frame(bytes, std::max<std::size_t>(bytes.size(), 1u));
    for (auto const& reply : whole.replies)
        checkReply(reply);
    check(sameFraming(whole, frame(bytes, 1u)));
    check(sameFraming(whole, frame(bytes, 1u + size % 7u)));
    return 0;
}

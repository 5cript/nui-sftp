#include <ftp/protocol/command.hpp>
#include <ftp/protocol/features.hpp>
#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    using namespace FileTransferProtocol;

    void check(bool condition)
    {
        if (!condition)
            std::abort();
    }

    /**
     * @brief A time-val reads back as the same text when it has whole seconds and no leap second.
     */
    void checkTimeVal(std::string_view bytes)
    {
        const auto time = parseTimeVal(bytes);
        if (time && bytes.size() == 14u && bytes.substr(12u, 2u) != "60")
            check(formatTimeVal(std::chrono::floor<std::chrono::seconds>(*time)) == bytes);
    }

    /**
     * @brief Any argument is one line on the wire or refused, and undoubling 0xFF gives it back.
     */
    void checkCommand(Verb verb, std::string_view bytes)
    {
        const Command command{.verb = verb, .argument = std::string{bytes}};
        static_cast<void>(command.toLogString());
        const auto encoded = command.encode();
        if (!encoded)
            return;
        check(encoded->ends_with("\r\n"));
        check(encoded->find_first_of(std::string_view{"\r\n\0", 3u}) == encoded->size() - 2u);
        if (bytes.empty())
            return;
        const auto name = wireName(verb);
        check(name.has_value());
        const auto prefix = name->size() + 1u;
        const auto sent = std::string_view{*encoded}.substr(prefix, encoded->size() - prefix - 2u);
        std::string argument{};
        for (std::size_t position = 0u; position < sent.size(); ++position)
        {
            argument += sent[position];
            if (sent[position] == '\xFF')
            {
                check(position + 1u < sent.size() && sent[position + 1u] == '\xFF');
                ++position;
            }
        }
        check(argument == bytes);
    }

    void checkReply(Reply const& reply)
    {
        static_cast<void>(parseSizeReply(reply));
        static_cast<void>(parseModificationTimeReply(reply));
        if (const auto endpoint = parsePassiveReply(reply))
            check(endpoint->port != 0u);
        if (const auto port = parseExtendedPassiveReply(reply))
            check(*port != 0u);
        if (const auto path = parsePathReply(reply, Verb::MakeDirectory))
            check(!path->empty());
        if (const auto features = Features::fromReply(reply))
        {
            for (auto const& feature : features->all())
            {
                check(!feature.name.empty());
                for (const auto character : feature.name)
                    check(character < 'a' || character > 'z');
            }
            static_cast<void>(features->has("AUTH", "TLS"));
            static_cast<void>(features->machineListFact("size"));
        }
        for (const auto verb : {std::optional<Verb>{}, std::optional{Verb::Retrieve}, std::optional{Verb::Password}})
        {
            const auto error = replyError(reply, verb);
            check(error.protocol == RemoteIo::Protocol::Ftp);
            check(error.protocolCode == reply.code);
        }
    }
}

/**
 * @brief Runs the typed reply parsers, FEAT and the error mapping on every reply framed from the input, and the
 *        time-val parser and command encoder on the input itself.
 */
extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    const std::string_view bytes{reinterpret_cast<char const*>(data), size};
    checkTimeVal(bytes);
    checkCommand(Verb::Retrieve, bytes);
    checkCommand(Verb::ExtendedPassive, bytes);

    ReplyFramer framer{};
    if (framer.feed(bytes))
        static_cast<void>(framer.finish());
    while (const auto reply = framer.next())
        checkReply(*reply);
    return 0;
}

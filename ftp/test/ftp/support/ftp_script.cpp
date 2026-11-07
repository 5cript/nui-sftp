#include <ftp/support/ftp_script.hpp>

#include <fmt/format.h>

#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief Ends every line of @p text in CRLF, the last one too.
         */
        std::string withLineEnds(std::string_view text)
        {
            std::string lines{};
            lines.reserve(text.size() + 2u);
            for (std::size_t position = 0u; position < text.size(); ++position)
            {
                if (text[position] == '\n' && (position == 0u || text[position - 1u] != '\r'))
                    lines += '\r';
                lines += text[position];
            }
            if (!lines.ends_with("\r\n"))
                lines += "\r\n";
            return lines;
        }
    }

    std::string fatalAlert()
    {
        // TLS 1.2 record: alert, length 2, level fatal, handshake_failure.
        return std::string{"\x15\x03\x03\x00\x02\x02\x28", 7u};
    }

    DataPlan& DataPlan::send(std::string bytes, std::size_t chunkSize, std::chrono::milliseconds interval)
    {
        actions.emplace_back(Send{.bytes = std::move(bytes), .chunkSize = chunkSize, .interval = interval});
        return *this;
    }
    DataPlan& DataPlan::sendForever()
    {
        actions.emplace_back(SendForever{});
        return *this;
    }
    DataPlan& DataPlan::expectUpload(std::size_t limit)
    {
        actions.emplace_back(ExpectUpload{.limit = limit});
        return *this;
    }
    DataPlan& DataPlan::close()
    {
        actions.emplace_back(Close{});
        return *this;
    }
    DataPlan& DataPlan::reset()
    {
        actions.emplace_back(Reset{});
        return *this;
    }
    DataPlan& DataPlan::stallAfter(std::string bytes)
    {
        actions.emplace_back(StallAfter{.bytes = std::move(bytes)});
        return *this;
    }
    DataPlan& DataPlan::tls(TlsBehaviour behaviour)
    {
        tlsBehaviour = std::move(behaviour);
        return *this;
    }
    DataPlan& DataPlan::announce(std::string address, std::uint16_t port)
    {
        announcedAddress = std::move(address);
        announcedPort = port;
        return *this;
    }
    DataPlan& DataPlan::announceAs(std::string reply)
    {
        announcedReply = std::move(reply);
        return *this;
    }
    DataPlan& DataPlan::acceptFromAnyone()
    {
        onlyControlPeer = false;
        return *this;
    }
    DataPlan& DataPlan::acceptOnlyFromControlPeer()
    {
        onlyControlPeer = true;
        return *this;
    }

    std::string listing(ListingFormat format, std::vector<ListingEntry> const& entries)
    {
        std::string text{};
        for (auto const& entry : entries)
        {
            switch (format)
            {
                case ListingFormat::Unix:
                    text += fmt::format(
                        "{}rwxr-xr-x 1 ftp ftp {:>10} Oct 01 12:34 {}\r\n",
                        entry.directory ? 'd' : '-',
                        entry.size,
                        entry.name
                    );
                    break;
                case ListingFormat::Dos:
                    text += entry.directory ? fmt::format("10-01-26  12:34PM       <DIR>          {}\r\n", entry.name)
                                            : fmt::format("10-01-26  12:34PM {:>20} {}\r\n", entry.size, entry.name);
                    break;
                case ListingFormat::Mlsd:
                    text += entry.directory
                        ? fmt::format("type=dir;modify=20261001123400; {}\r\n", entry.name)
                        : fmt::format("type=file;size={};modify=20261001123400; {}\r\n", entry.size, entry.name);
                    break;
            }
        }
        return text;
    }

    Script& Script::expect(std::string verb)
    {
        steps.emplace_back(Expect{.verb = std::move(verb)});
        return *this;
    }
    Script& Script::expect(std::string verb, std::string argumentPattern)
    {
        steps.emplace_back(Expect{.verb = std::move(verb), .argumentPattern = std::move(argumentPattern)});
        return *this;
    }
    Script& Script::reply(std::string_view text)
    {
        steps.emplace_back(Send{.bytes = withLineEnds(text)});
        return *this;
    }
    Script& Script::replyRaw(std::string bytes)
    {
        steps.emplace_back(Send{.bytes = std::move(bytes)});
        return *this;
    }
    Script& Script::unsolicited(std::string_view text)
    {
        return reply(text);
    }
    Script& Script::delay(std::chrono::milliseconds duration)
    {
        steps.emplace_back(Delay{.duration = duration});
        return *this;
    }
    Script& Script::dribble(std::string bytes, std::chrono::milliseconds interval)
    {
        steps.emplace_back(Dribble{.bytes = std::move(bytes), .interval = interval});
        return *this;
    }
    Script& Script::closeControl()
    {
        steps.emplace_back(CloseControl{});
        return *this;
    }
    Script& Script::resetControl()
    {
        steps.emplace_back(ResetControl{});
        return *this;
    }
    Script& Script::stall()
    {
        steps.emplace_back(Stall{});
        return *this;
    }
    Script& Script::expectDisconnect()
    {
        steps.emplace_back(ExpectDisconnect{});
        return *this;
    }
    Script& Script::startTls(TlsBehaviour behaviour)
    {
        steps.emplace_back(StartTls{.behaviour = std::move(behaviour)});
        return *this;
    }
    Script& Script::checkpoint(std::string name)
    {
        steps.emplace_back(Checkpoint{.name = std::move(name)});
        return *this;
    }
    Script& Script::passive(DataPlan plan)
    {
        steps.emplace_back(Passive{.plan = std::move(plan)});
        return *this;
    }
    Script& Script::beginData()
    {
        steps.emplace_back(BeginData{});
        return *this;
    }
    Script& Script::awaitData()
    {
        steps.emplace_back(AwaitData{});
        return *this;
    }
    Script& Script::stopData()
    {
        steps.emplace_back(StopData{});
        return *this;
    }

    Script& Script::login(std::string user, std::string password)
    {
        return reply("220 Scripted server ready")
            .expect("USER", std::move(user))
            .reply("331 Password required")
            .expect("PASS", std::move(password))
            .reply("230 Logged in");
    }

    Script& Script::featLike(ServerLike server)
    {
        expect("FEAT");
        switch (server)
        {
            case ServerLike::Vsftpd:
                return reply(
                    "211-Features:\n EPRT\n EPSV\n MDTM\n PASV\n PBSZ\n PROT\n REST STREAM\n SIZE\n TVFS\n211 End"
                );
            case ServerLike::Proftpd:
                return reply(
                    "211-Features:\n AUTH TLS\n CCC\n CLNT\n CSID\n EPRT\n EPSV\n HOST\n LANG en-US*\n MDTM\n"
                    " MFF modify;UNIX.group;UNIX.mode;\n MFMT\n"
                    " MLST modify*;perm*;size*;type*;unique*;UNIX.group*;UNIX.groupname*;UNIX.mode*;UNIX.owner*;"
                    "UNIX.ownername*;\n"
                    " PBSZ\n PROT\n RANG STREAM\n REST STREAM\n SITE COPY\n SITE MKDIR\n SITE RMDIR\n SITE SYMLINK\n"
                    " SITE UTIME\n SIZE\n SSCN\n TVFS\n UTF8\n211 End"
                );
            case ServerLike::PureFtpd:
                return reply(
                    "211-Extensions supported:\n UTF8\n EPRT\n IDLE\n MDTM\n SIZE\n MFMT\n REST STREAM\n"
                    " MLST type*;size*;sizd*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;unique*;\n"
                    " MLSD\n PRET\n AUTH TLS\n PBSZ\n PROT\n TVFS\n ESTA\n PASV\n EPSV\n ESTP\n211 End."
                );
            case ServerLike::Iis:
                return reply(
                    "211-Extended features supported:\n LANG EN*\n UTF8\n AUTH TLS;TLS-C;SSL;TLS-P;\n PBSZ\n"
                    " PROT C;P;\n CCC\n HOST\n SIZE\n MDTM\n REST STREAM\n211 END"
                );
        }
        return *this;
    }

    Script& Script::abortSequence(AbortReplies replies)
    {
        expect("ABOR");
        switch (replies)
        {
            case AbortReplies::VsftpdLike:
                return reply("426 Failure writing network stream.").reply("225 No transfer to ABOR.");
            case AbortReplies::ProftpdPlainLike:
                return reply("426 Transfer aborted. Data connection closed").reply("226 Abort successful");
            case AbortReplies::ProftpdTlsLike:
                return reply("226 Abort successful").reply("426 Transfer aborted. Data connection closed");
            case AbortReplies::PureFtpdLike:
                return reply("426 Transfer aborted").reply("226-Transfer aborted\n226 0.000 seconds");
            case AbortReplies::PureFtpdFailedLike:
                return reply("426 Transfer aborted").reply("451 Transfer aborted");
            case AbortReplies::FinalOnly:
                return reply("226 Transfer complete");
        }
        return *this;
    }
}

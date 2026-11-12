#include <ftp/protocol/command.hpp>

#include <boost/mp11/algorithm.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <ranges>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        constexpr std::array wireNames{
            std::pair{Verb::User, std::string_view{"USER"}},
            std::pair{Verb::Password, std::string_view{"PASS"}},
            std::pair{Verb::Account, std::string_view{"ACCT"}},
            std::pair{Verb::Authenticate, std::string_view{"AUTH"}},
            std::pair{Verb::ProtectionBufferSize, std::string_view{"PBSZ"}},
            std::pair{Verb::DataProtection, std::string_view{"PROT"}},
            std::pair{Verb::Features, std::string_view{"FEAT"}},
            std::pair{Verb::Options, std::string_view{"OPTS"}},
            std::pair{Verb::RepresentationType, std::string_view{"TYPE"}},
            std::pair{Verb::PrintWorkingDirectory, std::string_view{"PWD"}},
            std::pair{Verb::ChangeWorkingDirectory, std::string_view{"CWD"}},
            std::pair{Verb::ChangeToParentDirectory, std::string_view{"CDUP"}},
            std::pair{Verb::MakeDirectory, std::string_view{"MKD"}},
            std::pair{Verb::RemoveEmptyDirectory, std::string_view{"RMD"}},
            std::pair{Verb::Delete, std::string_view{"DELE"}},
            std::pair{Verb::RenameFrom, std::string_view{"RNFR"}},
            std::pair{Verb::RenameTo, std::string_view{"RNTO"}},
            std::pair{Verb::Size, std::string_view{"SIZE"}},
            std::pair{Verb::ModificationTime, std::string_view{"MDTM"}},
            std::pair{Verb::SetModificationTime, std::string_view{"MFMT"}},
            std::pair{Verb::Site, std::string_view{"SITE"}},
            std::pair{Verb::MachineListSingle, std::string_view{"MLST"}},
            std::pair{Verb::MachineListDirectory, std::string_view{"MLSD"}},
            std::pair{Verb::Status, std::string_view{"STAT"}},
            std::pair{Verb::ExtendedPassive, std::string_view{"EPSV"}},
            std::pair{Verb::Passive, std::string_view{"PASV"}},
            std::pair{Verb::Restart, std::string_view{"REST"}},
            std::pair{Verb::Retrieve, std::string_view{"RETR"}},
            std::pair{Verb::Store, std::string_view{"STOR"}},
            std::pair{Verb::Append, std::string_view{"APPE"}},
            std::pair{Verb::List, std::string_view{"LIST"}},
            std::pair{Verb::NameList, std::string_view{"NLST"}},
            std::pair{Verb::Abort, std::string_view{"ABOR"}},
            std::pair{Verb::Noop, std::string_view{"NOOP"}},
            std::pair{Verb::Quit, std::string_view{"QUIT"}},
        };
        static_assert(wireNames.size() == boost::mp11::mp_size<boost::describe::describe_enumerators<Verb>>::value);
        // Looked up by the value of a Verb, so every entry sits at the index of its verb.
        static_assert(std::ranges::all_of(
            std::views::iota(std::size_t{0u}, wireNames.size()),
            [](std::size_t index)
            {
                return wireNames[index].first == static_cast<Verb>(index);
            }
        ));

        constexpr char interpretAsCommand = '\xFF';

        RemoteIo::RemoteError refused(std::string_view name, std::string_view reason)
        {
            return RemoteIo::RemoteError{
                .kind = RemoteIo::RemoteErrorKind::Failure,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = fmt::format("{} was not sent, {}", name, reason),
                .wrapperError = RemoteIo::WrapperErrors::InvalidPath,
            };
        }

        std::string unknownVerb(Verb verb)
        {
            return fmt::format("unknown verb {}", static_cast<int>(verb));
        }
    }

    std::optional<std::string_view> wireName(Verb verb)
    {
        const auto index = static_cast<std::size_t>(verb);
        if (index >= wireNames.size())
            return std::nullopt;
        return wireNames[index].second;
    }

    std::expected<std::string, RemoteIo::RemoteError> Command::encode() const
    {
        const auto name = wireName(verb);
        if (!name)
        {
            assert(false && "Command::encode() with a verb that is no Verb");
            return std::unexpected(
                RemoteIo::RemoteError{
                    .kind = RemoteIo::RemoteErrorKind::Failure,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message = fmt::format("Nothing was sent, {}", unknownVerb(verb)),
                }
            );
        }
        if (argument.find_first_of(std::string_view{"\r\n\0", 3u}) != std::string::npos)
            return std::unexpected(refused(*name, "its argument holds a line break or a NUL byte"));
        if (verb == Verb::ExtendedPassive && !argument.empty())
            return std::unexpected(refused(*name, "the client sends it without an argument"));

        std::string encoded{};
        encoded.reserve(name->size() + argument.size() + 3u);
        encoded += *name;
        if (!argument.empty())
        {
            encoded += ' ';
            for (const auto character : argument)
            {
                if (character == interpretAsCommand)
                    encoded += interpretAsCommand;
                encoded += character;
            }
        }
        encoded += "\r\n";
        return encoded;
    }

    std::string Command::toLogString() const
    {
        const auto name = wireName(verb);
        if (!name)
            return unknownVerb(verb);
        return std::string{*name};
    }
}

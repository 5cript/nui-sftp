#pragma once

#include <ftp/protocol/command.hpp>

#include <boost/mp11/algorithm.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        std::set<std::string_view> allWireNames()
        {
            std::set<std::string_view> names{};
            boost::mp11::mp_for_each<boost::describe::describe_enumerators<Verb>>(
                [&names](auto enumerator)
                {
                    const auto name = wireName(enumerator.value);
                    EXPECT_TRUE(name) << enumerator.name;
                    if (name)
                        names.insert(*name);
                }
            );
            return names;
        }

        std::expected<std::string, RemoteIo::RemoteError> encoded(Verb verb, std::string argument = {})
        {
            return Command{.verb = verb, .argument = std::move(argument)}.encode();
        }
    }

    TEST(CommandTests, VerbAndArgumentAreSeparatedByOneSpaceAndEndInCrLf)
    {
        EXPECT_EQ(encoded(Verb::User, "anonymous"), "USER anonymous\r\n");
        EXPECT_EQ(encoded(Verb::Retrieve, "dir/file.txt"), "RETR dir/file.txt\r\n");
    }

    TEST(CommandTests, CommandWithoutArgumentIsTheVerbAlone)
    {
        EXPECT_EQ(encoded(Verb::PrintWorkingDirectory), "PWD\r\n");
        EXPECT_EQ(encoded(Verb::ExtendedPassive), "EPSV\r\n");
    }

    TEST(CommandTests, ArgumentIsSentAsItIs)
    {
        EXPECT_EQ(encoded(Verb::Store, " leading space.txt"), "STOR  leading space.txt\r\n");
        EXPECT_EQ(encoded(Verb::Delete, "trailing "), "DELE trailing \r\n");
        EXPECT_EQ(encoded(Verb::MakeDirectory, "ünïcödé-名前"), "MKD ünïcödé-名前\r\n");
    }

    TEST(CommandTests, InterpretAsCommandInTheArgumentIsDoubled)
    {
        EXPECT_EQ(encoded(Verb::Delete, "caf\xFF"), "DELE caf\xFF\xFF\r\n");
    }

    TEST(CommandTests, LineBreaksAndNulInTheArgumentAreRefused)
    {
        for (
            auto const& argument :
            std::vector<std::string>{"a\rb", "a\nb", "name\r\nDELE other", std::string{"a\0b", 3u}}
        )
        {
            SCOPED_TRACE(argument);
            const auto refused = encoded(Verb::Retrieve, argument);
            ASSERT_FALSE(refused);
            EXPECT_EQ(refused.error().wrapperError, RemoteIo::WrapperErrors::InvalidPath);
            EXPECT_EQ(refused.error().protocol, RemoteIo::Protocol::Ftp);
            EXPECT_EQ(refused.error().message.find(argument), std::string::npos);
        }
    }

    TEST(CommandTests, ExtendedPassiveTakesNoArgument)
    {
        const auto all = encoded(Verb::ExtendedPassive, "ALL");
        ASSERT_FALSE(all);
        EXPECT_EQ(all.error().wrapperError, RemoteIo::WrapperErrors::InvalidPath);
    }

    TEST(CommandTests, EveryVerbHasItsOwnWireName)
    {
        const auto names = allWireNames();
        EXPECT_EQ(names.size(), boost::mp11::mp_size<boost::describe::describe_enumerators<Verb>>::value);
        for (const auto name : names)
        {
            EXPECT_TRUE(name.size() == 3u || name.size() == 4u) << name;
            for (const auto character : name)
                EXPECT_TRUE(character >= 'A' && character <= 'Z') << name;
        }
    }

    TEST(CommandTests, CommandsTheClientMustNeverSendCannotBeEncoded)
    {
        const auto names = allWireNames();
        for (std::string_view forbidden : {"STRU", "MODE", "CCC", "PORT", "EPRT", "REIN", "MIC", "ENC", "CONF"})
            EXPECT_FALSE(names.contains(forbidden)) << forbidden;
    }

    TEST(CommandTests, WireNamesBelongToTheirVerbs)
    {
        EXPECT_EQ(wireName(Verb::User), "USER");
        EXPECT_EQ(wireName(Verb::Retrieve), "RETR");
        EXPECT_EQ(wireName(Verb::MachineListDirectory), "MLSD");
        EXPECT_EQ(wireName(Verb::Quit), "QUIT");
    }

    TEST(CommandTests, ValueThatIsNoVerbHasNoWireName)
    {
        EXPECT_EQ(wireName(static_cast<Verb>(999)), std::nullopt);
        EXPECT_EQ(wireName(static_cast<Verb>(-1)), std::nullopt);
    }

    TEST(CommandTests, LogStringIsTheVerbAlone)
    {
        EXPECT_EQ((Command{.verb = Verb::Password, .argument = "secret"}.toLogString()), "PASS");
        EXPECT_EQ((Command{.verb = Verb::Account, .argument = "billing"}.toLogString()), "ACCT");
        EXPECT_EQ((Command{.verb = Verb::User, .argument = "alice"}.toLogString()), "USER");
        EXPECT_EQ((Command{.verb = Verb::Retrieve, .argument = "/home/alice/private.txt"}.toLogString()), "RETR");
        EXPECT_EQ((Command{.verb = Verb::Noop}.toLogString()), "NOOP");
        EXPECT_EQ((Command{.verb = static_cast<Verb>(999)}.toLogString()), "unknown verb 999");
    }

    TEST(CommandDeathTests, EncodingAValueThatIsNoVerbIsACodingError)
    {
        // A release build has no assert and refuses to send.
        EXPECT_DEBUG_DEATH(
            {
                const auto refused = encoded(static_cast<Verb>(999), "x");
                EXPECT_FALSE(refused);
            },
            "no Verb"
        );
    }
}

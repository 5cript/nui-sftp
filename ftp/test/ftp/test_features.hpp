#pragma once

#include <ftp/protocol/features.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        // The FEAT replies of vsftpd 3.0.5, proftpd 1.3.8c and pure-ftpd 1.0.50 before login.
        const Reply vsftpdFeatures{
            .code = 211,
            .lines = {
                "211-Features:",
                " EPRT",
                " EPSV",
                " MDTM",
                " PASV",
                " PBSZ",
                " PROT",
                " REST STREAM",
                " SIZE",
                " TVFS",
                "211 End"
            },
        };
        const Reply proftpdFeatures{
            .code = 211,
            .lines = {
                "211-Features:",
                " AUTH TLS",
                " CCC",
                " EPSV",
                " LANG en-US*",
                " MDTM",
                " MFF modify;UNIX.group;UNIX.mode;",
                " MFMT",
                " MLST modify*;perm*;size*;type*;unique*;UNIX.group*;UNIX.groupname*;UNIX.mode*;UNIX.owner*;"
                "UNIX.ownername*;",
                " REST STREAM",
                " SITE UTIME",
                " SIZE",
                " UTF8",
                "211 End"
            },
        };
        const Reply pureFtpdFeatures{
            .code = 211,
            .lines = {
                "211-Extensions supported:",
                " UTF8",
                " MLST type*;size*;sizd*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;unique*;",
                " MLSD",
                " AUTH TLS",
                " PBSZ",
                " EPSV",
                "211 End."
            },
        };
    }

    TEST(FeaturesTests, VsftpdFeatures)
    {
        const auto features = Features::fromReply(vsftpdFeatures);
        ASSERT_TRUE(features);
        EXPECT_EQ(features->all().size(), 9u);
        EXPECT_TRUE(features->has("EPSV"));
        EXPECT_TRUE(features->has("REST", "STREAM"));
        EXPECT_FALSE(features->has("MLST"));
        EXPECT_FALSE(features->has("AUTH"));
        EXPECT_FALSE(features->has("UTF8"));
    }

    TEST(FeaturesTests, ProftpdFeatures)
    {
        const auto features = Features::fromReply(proftpdFeatures);
        ASSERT_TRUE(features);
        EXPECT_TRUE(features->has("AUTH", "TLS"));
        EXPECT_TRUE(features->has("MFMT"));
        EXPECT_TRUE(features->has("SITE", "UTIME"));
        EXPECT_EQ(
            features->parametersOf("MLST"),
            "modify*;perm*;size*;type*;unique*;UNIX.group*;UNIX.groupname*;UNIX.mode*;UNIX.owner*;UNIX.ownername*;"
        );
        EXPECT_EQ(features->machineListFact("size"), FactState::Enabled);
        EXPECT_EQ(features->machineListFact("UNIX.mode"), FactState::Enabled);
        EXPECT_EQ(features->machineListFact("media-type"), std::nullopt);
        EXPECT_FALSE(features->has("MLSD"));
    }

    TEST(FeaturesTests, FactsOfMachineListingAreOfferedOrEnabled)
    {
        const auto features = Features::fromReply(
            Reply{.code = 211, .lines = {"211-Features:", " MLST Type*;Size;modify*;perm;", "211 End"}}
        );
        ASSERT_TRUE(features);
        EXPECT_EQ(features->machineListFact("type"), FactState::Enabled);
        EXPECT_EQ(features->machineListFact("SIZE"), FactState::Offered);
        EXPECT_EQ(features->machineListFact("Modify"), FactState::Enabled);
        EXPECT_EQ(features->machineListFact("perm"), FactState::Offered);
        EXPECT_EQ(features->machineListFact("unique"), std::nullopt);
        EXPECT_EQ(features->machineListFact(""), std::nullopt);
    }

    TEST(FeaturesTests, NoMachineListingHasNoFacts)
    {
        const auto features = Features::fromReply(vsftpdFeatures);
        ASSERT_TRUE(features);
        EXPECT_EQ(features->machineListFact("size"), std::nullopt);
    }

    TEST(FeaturesTests, PureFtpdFeatures)
    {
        const auto features = Features::fromReply(pureFtpdFeatures);
        ASSERT_TRUE(features);
        EXPECT_TRUE(features->has("MLSD"));
        EXPECT_TRUE(features->has("UTF8"));
        EXPECT_EQ(features->parametersOf("MLSD"), "");
        EXPECT_EQ(features->all().front().name, "UTF8");
    }

    TEST(FeaturesTests, NamesAndParametersCompareInAnyCase)
    {
        const Reply reply{.code = 211, .lines = {"211-Features:", " mdtm", " Auth tls;ssl", " rest Stream", "211 End"}};
        const auto features = Features::fromReply(reply);
        ASSERT_TRUE(features);
        EXPECT_TRUE(features->has("MDTM"));
        EXPECT_TRUE(features->has("mdtm"));
        EXPECT_TRUE(features->has("AUTH", "TLS"));
        EXPECT_TRUE(features->has("auth", "SSL"));
        EXPECT_TRUE(features->has("REST", "STREAM"));
        EXPECT_FALSE(features->has("AUTH", "TL"));
        EXPECT_EQ(features->parametersOf("auth"), "tls;ssl");
    }

    TEST(FeaturesTests, ExtraSpacesAroundAFeatureAreDropped)
    {
        const Reply reply{.code = 211, .lines = {"211-Features:", "   SIZE  ", "  REST   STREAM ", " ", "211 End"}};
        const auto features = Features::fromReply(reply);
        ASSERT_TRUE(features);
        ASSERT_EQ(features->all().size(), 2u);
        EXPECT_EQ(features->parametersOf("SIZE"), "");
        EXPECT_EQ(features->parametersOf("REST"), "STREAM");
    }

    TEST(FeaturesTests, LinesWithoutALeadingSpaceAreNoFeatures)
    {
        const Reply reply{.code = 211, .lines = {"211-Features:", "211-MDTM", "SIZE", " EPSV", "211 End"}};
        const auto features = Features::fromReply(reply);
        ASSERT_TRUE(features);
        ASSERT_EQ(features->all().size(), 1u);
        EXPECT_EQ(features->all().front().name, "EPSV");
    }

    TEST(FeaturesTests, SingleLineReplyListsNothing)
    {
        const auto features = Features::fromReply(Reply{.code = 211, .lines = {"211 No features"}});
        ASSERT_TRUE(features);
        EXPECT_TRUE(features->all().empty());
    }

    TEST(FeaturesTests, NoFeatCommandMeansNoFeatures)
    {
        for (const auto code : {500, 502})
        {
            const auto features =
                Features::fromReply(Reply{.code = code, .lines = {fmt::format("{} Unknown command", code)}});
            ASSERT_TRUE(features);
            EXPECT_TRUE(features->all().empty());
        }
    }

    TEST(FeaturesTests, OtherRepliesAreErrors)
    {
        const auto refused = Features::fromReply(Reply{.code = 530, .lines = {"530 Please login with USER and PASS."}});
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().kind, RemoteIo::RemoteErrorKind::AuthenticationFailed);
        const auto unexpected = Features::fromReply(Reply{.code = 200, .lines = {"200 OK"}});
        ASSERT_FALSE(unexpected);
        EXPECT_EQ(unexpected.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
    }
}

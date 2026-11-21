#pragma once

#include <backend/ftp/certificate_trust.hpp>
#include <backend/ftp/certificate_trust_store.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace CertificateTrustTesting
    {
        FileTransferProtocol::Sha256Digest keyOf(std::uint8_t byte)
        {
            FileTransferProtocol::Sha256Digest key{};
            key.fill(byte);
            return key;
        }

        /**
         * @brief Keeps the answers a prompt was given.
         */
        struct Answers
        {
            std::vector<FtpConnect::TrustAnswer> given{};

            std::function<void(FtpConnect::TrustAnswer)> taker()
            {
                return [this](FtpConnect::TrustAnswer answer)
                {
                    given.push_back(answer);
                };
            }
        };
    }

    class CertificateTrustStoreTests : public ::testing::Test
    {
      protected:
        std::filesystem::path file() const
        {
            return directory_.path() / "trusted_keys.json";
        }

        Utility::TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(CertificateTrustStoreTests, NoKeyIsTrustedAtFirst)
    {
        const FtpConnect::CertificateTrustStore store{file()};
        EXPECT_FALSE(store.trustedKeyOf("ftp.example.com", 21u));
        EXPECT_FALSE(std::filesystem::exists(file())) << "saved without a key to save";
    }

    TEST_F(CertificateTrustStoreTests, AKeyOfThisRunIsTrustedAndNotSaved)
    {
        using namespace CertificateTrustTesting;
        {
            FtpConnect::CertificateTrustStore store{file()};
            store.trustForThisRun("ftp.example.com", 21u, keyOf(1u));
            const auto trusted = store.trustedKeyOf("ftp.example.com", 21u);
            ASSERT_TRUE(trusted);
            EXPECT_EQ(trusted->publicKey, keyOf(1u));
            EXPECT_FALSE(trusted->lasting);
        }
        const FtpConnect::CertificateTrustStore next{file()};
        EXPECT_FALSE(next.trustedKeyOf("ftp.example.com", 21u));
    }

    TEST_F(CertificateTrustStoreTests, ALastingKeyIsTrustedInTheNextRunToo)
    {
        using namespace CertificateTrustTesting;
        {
            FtpConnect::CertificateTrustStore store{file()};
            const auto saved = store.trustLastingly("ftp.example.com", 990u, keyOf(2u));
            ASSERT_TRUE(saved) << saved.error();
        }
        const FtpConnect::CertificateTrustStore next{file()};
        const auto trusted = next.trustedKeyOf("ftp.example.com", 990u);
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, keyOf(2u));
        EXPECT_TRUE(trusted->lasting);
    }

    TEST_F(CertificateTrustStoreTests, AKeyBelongsToOneHostAndPort)
    {
        using namespace CertificateTrustTesting;
        FtpConnect::CertificateTrustStore store{file()};
        ASSERT_TRUE(store.trustLastingly("ftp.example.com", 21u, keyOf(3u)));
        EXPECT_FALSE(store.trustedKeyOf("ftp.example.com", 990u));
        EXPECT_FALSE(store.trustedKeyOf("ftp.example.org", 21u));
        EXPECT_FALSE(store.trustedKeyOf("sub.ftp.example.com", 21u));
    }

    TEST_F(CertificateTrustStoreTests, HostNamesCompareWithoutCase)
    {
        using namespace CertificateTrustTesting;
        FtpConnect::CertificateTrustStore store{file()};
        ASSERT_TRUE(store.trustLastingly("FTP.Example.com", 21u, keyOf(4u)));
        const auto trusted = store.trustedKeyOf("ftp.example.COM", 21u);
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, keyOf(4u));
    }

    TEST_F(CertificateTrustStoreTests, TheKeyOfThisRunComesBeforeTheSavedOneWhichStaysSaved)
    {
        using namespace CertificateTrustTesting;
        {
            FtpConnect::CertificateTrustStore store{file()};
            ASSERT_TRUE(store.trustLastingly("ftp.example.com", 21u, keyOf(5u)));
            store.trustForThisRun("ftp.example.com", 21u, keyOf(6u));
            const auto trusted = store.trustedKeyOf("ftp.example.com", 21u);
            ASSERT_TRUE(trusted);
            EXPECT_EQ(trusted->publicKey, keyOf(6u));
            EXPECT_FALSE(trusted->lasting);
        }
        const FtpConnect::CertificateTrustStore next{file()};
        const auto trusted = next.trustedKeyOf("ftp.example.com", 21u);
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, keyOf(5u));
    }

    TEST_F(CertificateTrustStoreTests, ALastingKeyTakesThePlaceOfTheKeyOfThisRun)
    {
        using namespace CertificateTrustTesting;
        FtpConnect::CertificateTrustStore store{file()};
        store.trustForThisRun("ftp.example.com", 21u, keyOf(7u));
        ASSERT_TRUE(store.trustLastingly("ftp.example.com", 21u, keyOf(8u)));
        const auto trusted = store.trustedKeyOf("ftp.example.com", 21u);
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, keyOf(8u));
        EXPECT_TRUE(trusted->lasting);
    }

    TEST_F(CertificateTrustStoreTests, AForgottenKeyIsTrustedNeitherNowNorInTheNextRun)
    {
        using namespace CertificateTrustTesting;
        {
            FtpConnect::CertificateTrustStore store{file()};
            ASSERT_TRUE(store.trustLastingly("ftp.example.com", 21u, keyOf(9u)));
            ASSERT_TRUE(store.trustLastingly("other.example.com", 21u, keyOf(10u)));
            store.trustForThisRun("ftp.example.com", 21u, keyOf(11u));
            const auto forgotten = store.forget("ftp.example.com", 21u);
            ASSERT_TRUE(forgotten) << forgotten.error();
            EXPECT_FALSE(store.trustedKeyOf("ftp.example.com", 21u));
        }
        const FtpConnect::CertificateTrustStore next{file()};
        EXPECT_FALSE(next.trustedKeyOf("ftp.example.com", 21u));
        EXPECT_TRUE(next.trustedKeyOf("other.example.com", 21u));
    }

    TEST_F(CertificateTrustStoreTests, AFileThatCannotBeReadTrustsNoKeyAndIsKept)
    {
        using namespace CertificateTrustTesting;
        const std::string content =
            R"({"servers": [{"host": "ftp.example.com", "port": 21, "publicKeySha256": "01"}]})";
        {
            std::ofstream writer{file(), std::ios_base::binary};
            writer << content;
        }
        FtpConnect::CertificateTrustStore store{file()};
        EXPECT_FALSE(store.trustedKeyOf("ftp.example.com", 21u));

        ASSERT_TRUE(store.trustLastingly("ftp.example.com", 21u, keyOf(12u)));
        auto kept = file();
        kept += ".unreadable";
        std::ifstream reader{kept, std::ios_base::binary};
        ASSERT_TRUE(reader.good()) << "the unreadable file was not kept";
        const std::string keptContent{std::istreambuf_iterator<char>{reader}, std::istreambuf_iterator<char>{}};
        EXPECT_EQ(keptContent, content);
    }

    TEST_F(CertificateTrustStoreTests, AKeyThatCannotBeSavedIsTrustedForThisRun)
    {
        using namespace CertificateTrustTesting;
        // A file where the directory of the store would be.
        const auto blocker = directory_.path() / "blocker";
        {
            std::ofstream writer{blocker, std::ios_base::binary};
            writer << "in the way";
        }
        FtpConnect::CertificateTrustStore store{blocker / "trusted_keys.json"};
        const auto saved = store.trustLastingly("ftp.example.com", 21u, keyOf(13u));
        ASSERT_FALSE(saved);
        const auto trusted = store.trustedKeyOf("ftp.example.com", 21u);
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, keyOf(13u));
        EXPECT_FALSE(trusted->lasting);
    }

    TEST(CertificateTrustStoreMemoryTests, WithoutAFileLastingKeysAreKeptForTheRun)
    {
        using namespace CertificateTrustTesting;
        FtpConnect::CertificateTrustStore store{};
        const auto saved = store.trustLastingly("ftp.example.com", 21u, keyOf(14u));
        ASSERT_TRUE(saved) << saved.error();
        const auto trusted = store.trustedKeyOf("ftp.example.com", 21u);
        ASSERT_TRUE(trusted);
        EXPECT_TRUE(trusted->lasting);
    }

    TEST(PendingTrustPromptTests, AnAnswerGivesTheDecisionItNames)
    {
        using namespace CertificateTrustTesting;
        using FtpConnect::TrustAnswer;
        const std::vector<std::pair<std::string, TrustAnswer>> decisions{
            {"once", TrustAnswer::Once},
            {"always", TrustAnswer::Always},
            {"refuse", TrustAnswer::Refused},
        };
        for (auto const& [decision, expected] : decisions)
        {
            FtpConnect::PendingTrustPrompt pending{};
            Answers answers{};
            const auto promptId = pending.begin(answers.taker());
            EXPECT_TRUE(pending.answer(promptId, decision));
            EXPECT_EQ(answers.given, (std::vector<TrustAnswer>{expected})) << decision;
        }
    }

    TEST(PendingTrustPromptTests, AnAnswerThatIsNoDecisionRefuses)
    {
        using namespace CertificateTrustTesting;
        using FtpConnect::TrustAnswer;
        // What a forged answer could try: a key, a certificate fingerprint, other words for yes.
        const std::vector<std::string> forged{
            "",
            "yes",
            "true",
            "Once",
            "always ",
            FileTransferProtocol::formatDigest(keyOf(1u)),
            std::string(64u, 'a'),
        };
        for (auto const& decision : forged)
        {
            FtpConnect::PendingTrustPrompt pending{};
            Answers answers{};
            const auto promptId = pending.begin(answers.taker());
            EXPECT_TRUE(pending.answer(promptId, decision));
            EXPECT_EQ(answers.given, (std::vector<TrustAnswer>{TrustAnswer::Refused})) << "'" << decision << "'";
        }
    }

    TEST(PendingTrustPromptTests, AnAnswerToAnotherPromptIsDropped)
    {
        using namespace CertificateTrustTesting;
        using FtpConnect::TrustAnswer;
        FtpConnect::PendingTrustPrompt pending{};
        Answers first{};
        const auto firstId = pending.begin(first.taker());
        ASSERT_TRUE(pending.answer(firstId, "refuse"));

        Answers second{};
        const auto secondId = pending.begin(second.taker());
        ASSERT_NE(firstId, secondId);
        // The answer of the first dialog, late or sent again, does not trust the second certificate.
        EXPECT_FALSE(pending.answer(firstId, "always"));
        EXPECT_TRUE(second.given.empty());
        EXPECT_TRUE(pending.answer(secondId, "once"));
        EXPECT_EQ(second.given, (std::vector<TrustAnswer>{TrustAnswer::Once}));
    }

    TEST(PendingTrustPromptTests, APromptIsAnsweredOnce)
    {
        using namespace CertificateTrustTesting;
        using FtpConnect::TrustAnswer;
        FtpConnect::PendingTrustPrompt pending{};
        Answers answers{};
        const auto promptId = pending.begin(answers.taker());
        ASSERT_TRUE(pending.answer(promptId, "refuse"));
        EXPECT_FALSE(pending.answer(promptId, "always"));
        EXPECT_EQ(answers.given, (std::vector<TrustAnswer>{TrustAnswer::Refused}));
    }

    TEST(PendingTrustPromptTests, AnAnswerWithoutAPromptIsDropped)
    {
        FtpConnect::PendingTrustPrompt pending{};
        EXPECT_FALSE(pending.answer(0, "always"));
        EXPECT_FALSE(pending.answer(1, "always"));
    }

    TEST(PendingTrustPromptTests, ANewPromptRefusesTheOneStillWaiting)
    {
        using namespace CertificateTrustTesting;
        using FtpConnect::TrustAnswer;
        FtpConnect::PendingTrustPrompt pending{};
        Answers first{};
        const auto firstId = pending.begin(first.taker());
        Answers second{};
        const auto secondId = pending.begin(second.taker());
        EXPECT_EQ(first.given, (std::vector<TrustAnswer>{TrustAnswer::Refused}));
        EXPECT_FALSE(pending.answer(firstId, "always"));
        EXPECT_TRUE(pending.answer(secondId, "always"));
        EXPECT_EQ(second.given, (std::vector<TrustAnswer>{TrustAnswer::Always}));
    }
}

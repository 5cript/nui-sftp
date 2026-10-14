#pragma once

#include "common_fixture.hpp"
#include <ssh/key_file.hpp>
#include <ssh/ssh_key.hpp>
#include <paths/native.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <libssh/libssh.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace SecureShell::Test
{
    TEST(KeyFileTests, SplitsAnOpenSshPublicKeyLine)
    {
        const auto line = Detail::splitPublicKeyLine("ssh-ed25519 AAAAC3Nza comment with spaces\n");
        ASSERT_TRUE(line.has_value());
        EXPECT_EQ(line->first, "ssh-ed25519");
        EXPECT_EQ(line->second, "AAAAC3Nza");
        EXPECT_FALSE(Detail::splitPublicKeyLine("-----BEGIN PUBLIC KEY-----\nMIIC\n").has_value());
        EXPECT_FALSE(Detail::splitPublicKeyLine("unknown-type AAAA").has_value());
        EXPECT_FALSE(Detail::splitPublicKeyLine("ssh-rsa").has_value());
        EXPECT_FALSE(Detail::splitPublicKeyLine("").has_value());
    }

    TEST(KeyFileTests, ImportsKeysFromFilesWithNonAsciiNames)
    {
        const auto directory = temporaryRoot / Paths::nativeFromUtf8("Schlüssel 東京");
        std::filesystem::create_directories(directory);
        const auto privateFile = directory / Paths::nativeFromUtf8("ключ");
        {
            std::ofstream stream{privateFile, std::ios::binary};
            stream.write(privateKey, static_cast<std::streamsize>(std::strlen(privateKey)));
        }

        SshKey imported{};
        ASSERT_EQ(
            importPrivateKeyFile(privateFile, nullptr, nullptr, nullptr, &imported.underlyingReferenced()), SSH_OK
        );
        ASSERT_TRUE(imported);

        char* blob = nullptr;
        ASSERT_EQ(ssh_pki_export_pubkey_base64(imported.underlying(), &blob), SSH_OK);
        const std::unique_ptr<char, decltype(&ssh_string_free_char)> ownedBlob{blob, &ssh_string_free_char};
        const auto publicFile = directory / Paths::nativeFromUtf8("ключ.pub");
        {
            std::ofstream stream{publicFile, std::ios::binary};
            stream << fmt::format(
                "{} {} test@host\n", ssh_key_type_to_char(ssh_key_type(imported.underlying())), ownedBlob.get()
            );
        }
        SshKey importedPublic{};
        ASSERT_EQ(importPublicKeyFile(publicFile, &importedPublic.underlyingReferenced()), SSH_OK);
        EXPECT_EQ(ssh_key_cmp(importedPublic.underlying(), imported.underlying(), SSH_KEY_CMP_PUBLIC), 0);
    }

    TEST(KeyFileTests, AMissingKeyFileIsReportedLikeLibssh)
    {
        SshKey key{};
        const auto missing = temporaryRoot / Paths::nativeFromUtf8("fehlt-ü");
        EXPECT_EQ(importPrivateKeyFile(missing, nullptr, nullptr, nullptr, &key.underlyingReferenced()), SSH_EOF);
        EXPECT_EQ(importPublicKeyFile(missing, &key.underlyingReferenced()), SSH_EOF);
    }
}

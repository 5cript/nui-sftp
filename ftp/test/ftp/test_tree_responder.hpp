#pragma once

#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/test_ftp_client.hpp>
#include <ftp/support/tree_responder.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief The commands a TreeResponder serves, from a client that knows nothing of the product code.
     */
    class TreeResponderTests : public ::testing::Test
    {
      protected:
        void serve(std::vector<ServedEntry> root, ServedFeatures features = {})
        {
            tree_ = std::make_shared<ServedTree>(std::move(root));
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started.has_value()) << started.error();
            server_ = std::move(started).value();
            server_->addConnection(serving(tree_, features));
            const auto connected = client_.connect(server_->address(), server_->port());
            ASSERT_TRUE(connected.has_value()) << connected.error();
            ASSERT_TRUE(client_.readReply().has_value());
            EXPECT_EQ(codeOf("USER user"), 331);
            EXPECT_EQ(codeOf("PASS password"), 230);
        }

        void TearDown() override
        {
            client_.close();
            if (!server_)
                return;
            EXPECT_TRUE(server_->waitUntilDisconnected());
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
        }

        int codeOf(std::string_view line)
        {
            const auto reply = client_.command(line);
            EXPECT_TRUE(reply.has_value()) << reply.error();
            if (!reply)
                return 0;
            return reply->code;
        }

        std::string textOf(std::string_view line)
        {
            const auto reply = client_.command(line);
            EXPECT_TRUE(reply.has_value()) << reply.error();
            if (!reply)
                return {};
            return reply->text;
        }

        /**
         * @brief Opens a data connection and sends @p line, which the server answers with a preliminary reply or
         *        refuses.
         *
         * @return The code of the reply to @p line.
         */
        std::expected<int, std::string> startTransfer(std::string_view line)
        {
            const auto passive = client_.command("EPSV");
            if (!passive)
                return std::unexpected(passive.error());
            const auto port = TestFtpClient::passivePort(*passive);
            if (!port)
                return std::unexpected(fmt::format("No port in '{}'", passive->text));
            if (auto opened = client_.openData(server_->address(), *port); !opened)
                return std::unexpected(std::move(opened).error());
            return client_.command(line).transform(
                [](TestFtpClient::Reply const& reply)
                {
                    return reply.code;
                }
            );
        }

        /**
         * @return The code of the final reply to @p line, which sends @p bytes.
         */
        std::expected<int, std::string> upload(std::string_view line, std::string_view bytes)
        {
            const auto started = startTransfer(line);
            if (!started)
                return started;
            if (*started != 150)
            {
                client_.closeData();
                return started;
            }
            if (auto written = client_.writeData(bytes); !written)
                return std::unexpected(std::move(written).error());
            client_.closeData();
            return client_.readReply().transform(
                [](TestFtpClient::Reply const& reply)
                {
                    return reply.code;
                }
            );
        }

        /**
         * @return What RETR of @p path sent, once the server confirmed it.
         */
        std::expected<std::string, std::string> download(std::string_view path)
        {
            const auto started = startTransfer(fmt::format("RETR {}", path));
            if (!started)
                return std::unexpected(started.error());
            if (*started != 150)
                return std::unexpected(fmt::format("RETR answered {}", *started));
            auto data = client_.readData();
            if (!data)
                return data;
            const auto done = client_.readReply();
            if (!done)
                return std::unexpected(done.error());
            if (done->code != 226)
                return std::unexpected(fmt::format("RETR ended with {}", done->code));
            return data;
        }

        std::shared_ptr<ServedTree> tree_{};
        std::unique_ptr<ScriptedFtpServer> server_{};
        TestFtpClient client_{};
    };

    TEST_F(TreeResponderTests, AStoredFileIsServedBack)
    {
        serve({servedDirectory("dir", {})});
        const auto stored = upload("STOR /dir/new.txt", "uploaded");
        ASSERT_TRUE(stored.has_value()) << stored.error();
        EXPECT_EQ(*stored, 226);
        EXPECT_EQ(tree_->contentOf("/dir/new.txt"), "uploaded");
        EXPECT_EQ(download("/dir/new.txt"), "uploaded");
        EXPECT_EQ(codeOf("SIZE /dir/new.txt"), 213);
        EXPECT_EQ(tree_->stored(), (std::vector<std::string>{"/dir/new.txt"}));
    }

    TEST_F(TreeResponderTests, AStoreReplacesAFileFromItsStart)
    {
        serve({servedFile("file.txt", "old content")});
        EXPECT_EQ(upload("STOR file.txt", "new"), 226);
        EXPECT_EQ(tree_->contentOf("/file.txt"), "new");
    }

    TEST_F(TreeResponderTests, AStoreBehindARestartKeepsWhatLiesBeforeAndBehindIt)
    {
        serve({servedFile("file.txt", "abcdef")});
        EXPECT_EQ(codeOf("REST 2"), 350);
        EXPECT_EQ(upload("STOR /file.txt", "XY"), 226);
        EXPECT_EQ(tree_->contentOf("/file.txt"), "abXYef");
        // The restart counts for one transfer.
        EXPECT_EQ(upload("STOR /file.txt", "new"), 226);
        EXPECT_EQ(tree_->contentOf("/file.txt"), "new");
    }

    TEST_F(TreeResponderTests, AnAppendWritesAtTheEndAndCreatesAMissingFile)
    {
        serve({servedFile("file.txt", "abc")});
        EXPECT_EQ(upload("APPE /file.txt", "def"), 226);
        EXPECT_EQ(tree_->contentOf("/file.txt"), "abcdef");
        EXPECT_EQ(upload("APPE /other.txt", "new"), 226);
        EXPECT_EQ(tree_->contentOf("/other.txt"), "new");
    }

    TEST_F(TreeResponderTests, AStoreIntoAMissingDirectoryOrOntoADirectoryIsRefused)
    {
        serve({servedDirectory("dir", {})});
        EXPECT_EQ(upload("STOR /missing/file.txt", "lost"), 553);
        EXPECT_EQ(upload("STOR /dir", "lost"), 553);
        EXPECT_FALSE(tree_->exists("/missing"));
        EXPECT_TRUE(tree_->stored().empty());
    }

    TEST_F(TreeResponderTests, AMakeDirectoryCreatesOnlyAMissingName)
    {
        serve({servedFile("file.txt", "content")});
        EXPECT_EQ(textOf("MKD /dir"), "257 \"/dir\" created");
        EXPECT_EQ(codeOf("CWD /dir"), 250);
        EXPECT_EQ(codeOf("MKD sub"), 257);
        EXPECT_TRUE(tree_->directoryAt("/dir/sub").has_value());
        EXPECT_EQ(codeOf("MKD /dir"), 550);
        EXPECT_EQ(codeOf("MKD /file.txt"), 550);
        EXPECT_EQ(codeOf("MKD /missing/dir"), 550);
    }

    TEST_F(TreeResponderTests, ARenameMovesAnEntryAndAFileReplacesAFile)
    {
        serve(
            {servedFile("a.txt", "alpha"),
                servedFile("b.txt", "beta"),
                servedDirectory("dir", {servedFile("inner.txt", "inner")}),
                servedDirectory("empty", {})}
        );
        EXPECT_EQ(codeOf("RNFR /a.txt"), 350);
        EXPECT_EQ(codeOf("RNTO /b.txt"), 250);
        EXPECT_FALSE(tree_->exists("/a.txt"));
        EXPECT_EQ(tree_->contentOf("/b.txt"), "alpha");

        EXPECT_EQ(codeOf("RNFR /dir"), 350);
        EXPECT_EQ(codeOf("RNTO /empty"), 250);
        EXPECT_EQ(tree_->contentOf("/empty/inner.txt"), "inner");
        EXPECT_FALSE(tree_->exists("/dir"));

        EXPECT_EQ(codeOf("RNFR /b.txt"), 350);
        EXPECT_EQ(codeOf("RNTO /empty/moved.txt"), 250);
        EXPECT_EQ(tree_->contentOf("/empty/moved.txt"), "alpha");
    }

    TEST_F(TreeResponderTests, ARenameRefusesWhatRenameRefuses)
    {
        serve(
            {servedFile("file.txt", "content"),
                servedDirectory("dir", {servedFile("inner.txt", "inner")}),
                servedDirectory("full", {servedFile("kept.txt", "kept")})}
        );
        EXPECT_EQ(codeOf("RNTO /new.txt"), 503);
        EXPECT_EQ(codeOf("RNFR /missing.txt"), 550);
        EXPECT_EQ(codeOf("RNTO /new.txt"), 503);

        const auto refused = [this](std::string_view from, std::string_view to)
        {
            EXPECT_EQ(codeOf(fmt::format("RNFR {}", from)), 350);
            EXPECT_EQ(codeOf(fmt::format("RNTO {}", to)), 550) << from << " to " << to;
        };
        refused("/file.txt", "/dir");
        refused("/dir", "/file.txt");
        refused("/dir", "/full");
        refused("/dir", "/dir/inner");
        refused("/file.txt", "/missing/file.txt");
        EXPECT_EQ(tree_->contentOf("/file.txt"), "content");
        EXPECT_EQ(tree_->contentOf("/dir/inner.txt"), "inner");
        EXPECT_EQ(tree_->contentOf("/full/kept.txt"), "kept");

        // An RNTO takes only the RNFR right before it.
        EXPECT_EQ(codeOf("RNFR /file.txt"), 350);
        EXPECT_EQ(codeOf("NOOP"), 200);
        EXPECT_EQ(codeOf("RNTO /new.txt"), 503);
        EXPECT_TRUE(tree_->exists("/file.txt"));
    }

    TEST_F(TreeResponderTests, ASetTimeIsWhatTheFileTellsAndLists)
    {
        serve({servedDirectory("dir", {servedFile("file.txt", "content")})});
        EXPECT_EQ(textOf("MDTM /dir/file.txt"), "213 20260102030405");
        EXPECT_EQ(textOf("MFMT 20231114221357 /dir/file.txt"), "213 Modify=20231114221357; /dir/file.txt");
        EXPECT_EQ(textOf("MDTM /dir/file.txt"), "213 20231114221357");
        EXPECT_EQ(codeOf("MFMT 20231314221357 /dir/file.txt"), 501);
        EXPECT_EQ(codeOf("MFMT 20231114221357 /dir/missing.txt"), 550);

        ASSERT_EQ(codeOf("CWD /dir"), 250);
        const auto started = startTransfer("LIST");
        ASSERT_TRUE(started.has_value()) << started.error();
        ASSERT_EQ(*started, 150);
        const auto listing = client_.readData();
        ASSERT_TRUE(listing.has_value()) << listing.error();
        EXPECT_NE(listing->find(" Nov 14  2023 file.txt\r\n"), std::string::npos) << *listing;
        const auto done = client_.readReply();
        ASSERT_TRUE(done.has_value()) << done.error();
        EXPECT_EQ(done->code, 226);

        // A write is a new version, the time it had is gone.
        EXPECT_EQ(upload("STOR /dir/file.txt", "new"), 226);
        EXPECT_EQ(textOf("MDTM /dir/file.txt"), "213 20260102030405");
    }

    TEST_F(TreeResponderTests, WithoutSizeNoCommandLooksUpASinglePath)
    {
        serve({servedFile("file.txt", "content")}, ServedFeatures{.size = false});
        const auto features = textOf("FEAT");
        EXPECT_EQ(features.find("SIZE"), std::string::npos) << features;
        EXPECT_EQ(features.find("MLST"), std::string::npos) << features;
        EXPECT_EQ(codeOf("SIZE /file.txt"), 502);
        EXPECT_EQ(codeOf("MLST /file.txt"), 500);
        EXPECT_EQ(download("/file.txt"), "content");
    }

    TEST_F(TreeResponderTests, FeaturesListSizeByDefault)
    {
        serve({servedFile("file.txt", "content")});
        const auto features = textOf("FEAT");
        EXPECT_NE(features.find(" SIZE"), std::string::npos) << features;
        EXPECT_NE(features.find(" MFMT"), std::string::npos) << features;
        EXPECT_NE(features.find(" REST STREAM"), std::string::npos) << features;
    }
}

#pragma once

#include <backend/file_system_answers.hpp>
#include <backend/rpc_helper.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>
#include <utility/user_directories.hpp>
#include "recording_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <string_view>
#include <vector>

namespace Test
{
    class FileSystemAnswerTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }

        void TearDown() override
        {
            processingThread_.stop();
        }

        static Paths::RemotePath remote(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }

        /**
         * @brief The first answer of @p reply, a failure when none came in time.
         */
        static RecordedAnswer answerOf(RecordingReply const& reply)
        {
            if (reply.answered().wait_for(std::chrono::seconds{5}) != std::future_status::ready)
            {
                ADD_FAILURE() << "the call was never answered";
                return {};
            }
            return reply.answered().get();
        }

        RemoteIo::ProcessingThread processingThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_{processingThread_.createStrand()};
        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> fileSystem_{
            std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                strand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{}
            )
        };
    };

    TEST_F(FileSystemAnswerTests, AListingAnswersWithTheEntriesOnceTheFileSystemCompletedIt)
    {
        fileSystem_->addFile(remote("/home/test/listed.txt"), "content");
        const RecordingReply reply{};

        fileSystem_->later(
            RemoteIo::ListDirectory{.path = remote("/home/test")},
            RpcHelper::answerOnCompletion(reply, FileSystemAnswers::listing("/home/test"))
        );

        const auto answer = answerOf(reply);
        ASSERT_TRUE(answer.json.has_value()) << answer.error.value_or("");
        auto const& entries = answer.json->at("entries");
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries.at(0).at("name"), "listed.txt");
        EXPECT_EQ(reply.answers(), 1u);
    }

    TEST_F(FileSystemAnswerTests, AListingOfAMissingDirectoryAnswersWithTheServersMessage)
    {
        const RecordingReply reply{};

        fileSystem_->later(
            RemoteIo::ListDirectory{.path = remote("/home/test/missing")},
            RpcHelper::answerOnCompletion(reply, FileSystemAnswers::listing("/home/test/missing"))
        );

        const auto answer = answerOf(reply);
        EXPECT_FALSE(answer.json.has_value());
        EXPECT_EQ(answer.error, "'/home/test/missing' does not exist");
    }

    TEST_F(FileSystemAnswerTests, AStatOfAFileAnswersWithIt)
    {
        fileSystem_->addFile(remote("/home/test/file.txt"), "content");
        const RecordingReply reply{};

        fileSystem_->later(
            RemoteIo::Stat{.path = remote("/home/test/file.txt")},
            RpcHelper::answerOnCompletion(reply, FileSystemAnswers::stat())
        );

        const auto answer = answerOf(reply);
        ASSERT_TRUE(answer.json.has_value()) << answer.error.value_or("");
        EXPECT_EQ(answer.json->at("success"), true);
        EXPECT_EQ(answer.json->at("stat").at("size"), 7u);
    }

    TEST_F(FileSystemAnswerTests, AStatOfAMissingFileAnswersWithNoStat)
    {
        const RecordingReply reply{};

        fileSystem_->later(
            RemoteIo::Stat{.path = remote("/home/test/missing.txt")},
            RpcHelper::answerOnCompletion(reply, FileSystemAnswers::stat())
        );

        const auto answer = answerOf(reply);
        ASSERT_TRUE(answer.json.has_value()) << answer.error.value_or("");
        EXPECT_EQ(*answer.json, (nlohmann::json{{"success", true}, {"stat", nullptr}}));
    }

    TEST_F(FileSystemAnswerTests, ACallTheProcessingThreadNeverRunsAnswersWithAnError)
    {
        processingThread_.stop();
        const RecordingReply reply{};

        fileSystem_->later(
            RemoteIo::Stat{.path = remote("/home/test/file.txt")},
            RpcHelper::answerOnCompletion(reply, FileSystemAnswers::stat())
        );

        ASSERT_EQ(reply.answered().wait_for(std::chrono::seconds{0}), std::future_status::ready)
            << "a refused call is answered before later returns";
        const auto answer = reply.answered().get();
        EXPECT_FALSE(answer.json.has_value());
        ASSERT_TRUE(answer.error.has_value());
        EXPECT_TRUE(answer.error->starts_with("Failed to stat file: ")) << *answer.error;
        EXPECT_EQ(reply.answers(), 1u);
    }

    TEST_F(FileSystemAnswerTests, ARenameOntoAnExistingNameAnswersThatTheTargetExists)
    {
        const RecordingReply reply{};

        FileSystemAnswers::renamed()(
            reply,
            RemoteIo::OperationResult<RemoteIo::Rename>{
                std::unexpected(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::AlreadyExists})
            }
        );

        const auto answer = answerOf(reply);
        EXPECT_EQ(answer.error, "backend.rename.targetExists");
    }

    TEST_F(FileSystemAnswerTests, PreDeleteChecksAnswerWithTheDirectoriesThatAreNotEmpty)
    {
        const RecordingReply reply{};

        FileSystemAnswers::preDeleteChecks()(
            reply,
            RemoteIo::OperationResult<RemoteIo::FilterOutEmptyDirectories>{
                std::vector<Paths::RemotePath>{remote("/home/test/full")}
            }
        );

        const auto answer = answerOf(reply);
        ASSERT_TRUE(answer.json.has_value()) << answer.error.value_or("");
        EXPECT_EQ(answer.json->at("nonEmptyDirectories"), (nlohmann::json{"/home/test/full"}));
    }

    TEST_F(FileSystemAnswerTests, UserDirectoriesAnswerWithTheirNamesAndPaths)
    {
        const RecordingReply reply{};

        FileSystemAnswers::userDirectories("/home/test")(
            reply,
            std::expected<std::vector<Utility::UserDirectory>, RemoteIo::RemoteError>{
                std::vector<Utility::UserDirectory>{{.name = "Documents", .path = "/home/test/Documents"}}
            }
        );

        const auto answer = answerOf(reply);
        ASSERT_TRUE(answer.json.has_value()) << answer.error.value_or("");
        EXPECT_EQ(
            answer.json->at("directories"), (nlohmann::json{{{"name", "Documents"}, {"path", "/home/test/Documents"}}})
        );
    }
}

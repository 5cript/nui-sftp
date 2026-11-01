#pragma once

#include "fake_remote_file_system.hpp"

#include <remote_io/async/processing_thread.hpp>
#include <remote_io/remote_file_system_helpers.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RemoteIo::Test
{
    namespace RemoteFileSystemHelperTestDetail
    {
        constexpr std::chrono::seconds helperReplyTimeout{5};

        inline Paths::RemotePath remotePath(std::string_view value)
        {
            return Paths::RemotePath::make(value).value();
        }

        /**
         * @brief What a file system's rename does, and whether it links, as the helpers meet it.
         */
        struct RenameBehaviour
        {
            std::string_view name;
            RenameSemantics semantics;
            /** @brief What the rename of a file system with Unknown semantics really does. */
            bool unknownRenameReplaces;
            bool hardLinks;
        };

        constexpr std::array renameBehaviours{
            RenameBehaviour{"Replaces", RenameSemantics::Replaces, true, false},
            RenameBehaviour{"ReplacesAndLinks", RenameSemantics::Replaces, true, true},
            RenameBehaviour{"Refuses", RenameSemantics::Refuses, false, false},
            RenameBehaviour{"RefusesAndLinks", RenameSemantics::Refuses, false, true},
            RenameBehaviour{"UnknownAndReplaces", RenameSemantics::Unknown, true, false},
            RenameBehaviour{"UnknownAndRefuses", RenameSemantics::Unknown, false, false},
        };

        inline const auto source = remotePath("/home/user/source.txt");
        inline const auto destination = remotePath("/home/user/destination.txt");
        inline const auto aside = remotePath("/home/user/destination.txt.aside");
    }

    using RemoteFileSystemHelperTestDetail::aside;
    using RemoteFileSystemHelperTestDetail::destination;
    using RemoteFileSystemHelperTestDetail::helperReplyTimeout;
    using RemoteFileSystemHelperTestDetail::remotePath;
    using RemoteFileSystemHelperTestDetail::RenameBehaviour;
    using RemoteFileSystemHelperTestDetail::renameBehaviours;
    using RemoteFileSystemHelperTestDetail::source;

    class RemoteFileSystemHelperFixture
    {
      protected:
        RemoteFileSystemHelperFixture()
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }
        ~RemoteFileSystemHelperFixture()
        {
            processingThread_.stop();
        }
        RemoteFileSystemHelperFixture(RemoteFileSystemHelperFixture const&) = delete;
        RemoteFileSystemHelperFixture& operator=(RemoteFileSystemHelperFixture const&) = delete;
        RemoteFileSystemHelperFixture(RemoteFileSystemHelperFixture&&) = delete;
        RemoteFileSystemHelperFixture& operator=(RemoteFileSystemHelperFixture&&) = delete;

        std::shared_ptr<FakeRemoteFileSystem> makeFileSystem(FakeRemoteFileSystem::Options options)
        {
            return std::make_shared<FakeRemoteFileSystem>(strand_.get(), std::move(options));
        }

        /**
         * @brief Runs @p helper on the processing thread and returns what it returned, a timeout error when it does
         *        not return in time.
         */
        template <typename FunctionT>
        static auto runInStrand(FakeRemoteFileSystem& fileSystem, FunctionT helper)
        {
            using ResultT = std::invoke_result_t<FunctionT>;
            auto result = fileSystem.performPromise(std::move(helper));
            if (result.wait_for(helperReplyTimeout) != std::future_status::ready)
                return ResultT{std::unexpected(RemoteError{.kind = RemoteErrorKind::TimedOut})};
            return result.get();
        }

        ProcessingThread processingThread_{};
        std::unique_ptr<ProcessingStrand> strand_{processingThread_.createStrand()};
    };

    class RemoteFileSystemRenameHelperTests
        : public RemoteFileSystemHelperFixture
        , public ::testing::TestWithParam<RenameBehaviour>
    {
      protected:
        void SetUp() override
        {
            fileSystem_ = makeFileSystem({
                .capabilities = {.renameSemantics = GetParam().semantics},
                .unknownRenameReplaces = GetParam().unknownRenameReplaces,
                .hardLinks = GetParam().hardLinks,
            });
            fileSystem_->addFile(source, "source");
        }

        std::expected<void, RemoteError>
        renameNoReplace(Paths::RemotePath const& from, std::optional<SharedData::FileType> sourceType = std::nullopt)
        {
            return runInStrand(
                *fileSystem_,
                [this, from, sourceType]()
                {
                    return renameNoReplaceInStrand(*fileSystem_, from, destination, sourceType);
                }
            );
        }

        std::expected<void, RemoteError> renameReplace()
        {
            return runInStrand(
                *fileSystem_,
                [this]()
                {
                    return renameReplaceInStrand(*fileSystem_, source, destination, ".aside");
                }
            );
        }

        std::shared_ptr<FakeRemoteFileSystem> fileSystem_{};
    };

    INSTANTIATE_TEST_SUITE_P(
        RenameBehaviours,
        RemoteFileSystemRenameHelperTests,
        ::testing::ValuesIn(renameBehaviours),
        [](::testing::TestParamInfo<RenameBehaviour> const& info)
        {
            return std::string{info.param.name};
        }
    );

    TEST_P(RemoteFileSystemRenameHelperTests, RenameNoReplaceMovesToAFreeName)
    {
        fileSystem_->addFile(remotePath("/home/user/folder/inner.txt"), "inner");

        ASSERT_TRUE(renameNoReplace(source).has_value());
        const auto movedFolder = runInStrand(
            *fileSystem_,
            [this]()
            {
                return renameNoReplaceInStrand(
                    *fileSystem_, remotePath("/home/user/folder"), remotePath("/home/user/moved")
                );
            }
        );
        ASSERT_TRUE(movedFolder.has_value()) << movedFolder.error().toString();

        EXPECT_FALSE(fileSystem_->exists(source));
        EXPECT_EQ(fileSystem_->contentOf(destination), "source");
        EXPECT_FALSE(fileSystem_->exists(remotePath("/home/user/folder")));
        EXPECT_EQ(fileSystem_->contentOf(remotePath("/home/user/moved/inner.txt")), "inner");
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameNoReplaceRefusesAnExistingDestination)
    {
        fileSystem_->addFile(destination, "destination");
        fileSystem_->addDirectory(remotePath("/home/user/folder"));

        for (const auto& from : {source, remotePath("/home/user/folder")})
        {
            const auto refused = renameNoReplace(from);
            ASSERT_FALSE(refused.has_value()) << from.value();
            EXPECT_EQ(refused.error().kind, RemoteErrorKind::AlreadyExists) << from.value();
        }
        EXPECT_EQ(fileSystem_->contentOf(source), "source");
        EXPECT_TRUE(fileSystem_->exists(remotePath("/home/user/folder")));
        EXPECT_EQ(fileSystem_->contentOf(destination), "destination");
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameNoReplaceNeverRenamesOverADestinationItCannotCheck)
    {
        fileSystem_->addFile(destination, "destination");
        fileSystem_->addDirectory(remotePath("/home/user/folder"));
        const auto denied = RemoteError{.kind = RemoteErrorKind::PermissionDenied};
        fileSystem_->failWhen<Lstat>(
            [](Lstat const& operation)
            {
                return operation.path == destination;
            },
            denied
        );
        fileSystem_->failWhen<LinkIntoPlace>(
            [](LinkIntoPlace const&)
            {
                return true;
            },
            denied
        );

        EXPECT_FALSE(renameNoReplace(source).has_value());
        EXPECT_FALSE(renameNoReplace(remotePath("/home/user/folder")).has_value());
        EXPECT_EQ(fileSystem_->contentOf(destination), "destination");
        EXPECT_EQ(fileSystem_->contentOf(source), "source");
        EXPECT_TRUE(fileSystem_->exists(remotePath("/home/user/folder")));
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameNoReplaceLinksAFileWhereTheFileSystemLinks)
    {
        ASSERT_TRUE(renameNoReplace(source, SharedData::FileType::Regular).has_value());

        EXPECT_EQ(fileSystem_->contentOf(destination), "source");
        EXPECT_EQ(fileSystem_->started<LinkIntoPlace>(), 1u);
        EXPECT_EQ(fileSystem_->started<Rename>(), GetParam().hardLinks ? 0u : 1u);
        EXPECT_EQ(fileSystem_->started<Lstat>(), 0u);
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameReplaceReplacesAnExistingFileOrMovesToAFreeName)
    {
        for (const bool destinationExists : {false, true})
        {
            fileSystem_->addFile(source, "source");
            if (destinationExists)
                fileSystem_->addFile(destination, "destination");

            const auto replaced = renameReplace();
            ASSERT_TRUE(replaced.has_value()) << replaced.error().toString();
            EXPECT_EQ(fileSystem_->contentOf(destination), "source") << destinationExists;
            EXPECT_FALSE(fileSystem_->exists(source)) << destinationExists;
            EXPECT_FALSE(fileSystem_->exists(aside)) << destinationExists;
        }
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameReplaceKeepsTheDestinationWhenTheRenameFails)
    {
        fileSystem_->addFile(destination, "destination");
        fileSystem_->failWhen<Rename>(
            [](Rename const& operation)
            {
                return operation.source == source;
            },
            RemoteError{.kind = RemoteErrorKind::NoSpace}
        );

        const auto failed = renameReplace();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().kind, RemoteErrorKind::NoSpace);
        EXPECT_EQ(fileSystem_->contentOf(destination), "destination");
        EXPECT_EQ(fileSystem_->contentOf(source), "source");
        EXPECT_FALSE(fileSystem_->exists(aside));
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameReplaceNamesWhereTheDestinationIsKeptWhenTheRestoreFails)
    {
        fileSystem_->addFile(destination, "destination");
        fileSystem_->failWhen<Rename>(
            [](Rename const& operation)
            {
                return operation.source == source || operation.source == aside;
            },
            RemoteError{.kind = RemoteErrorKind::ConnectionLost}
        );

        const auto failed = renameReplace();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().kind, RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(fileSystem_->contentOf(source), "source");
        if (fileSystem_->contentOf(destination) != "destination")
        {
            EXPECT_EQ(fileSystem_->contentOf(aside), "destination");
            EXPECT_NE(failed.error().message.find(aside.value()), std::string::npos) << failed.error().message;
        }
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameReplaceIsNotBlockedByALeftoverAside)
    {
        for (const bool destinationExists : {false, true})
        {
            fileSystem_->addFile(source, "source");
            fileSystem_->addFile(aside, "leftover");
            if (destinationExists)
                fileSystem_->addFile(destination, "destination");

            const auto replaced = renameReplace();
            ASSERT_TRUE(replaced.has_value()) << replaced.error().toString();
            EXPECT_EQ(fileSystem_->contentOf(destination), "source") << destinationExists;
            EXPECT_FALSE(fileSystem_->exists(source)) << destinationExists;
        }
    }

    TEST_P(RemoteFileSystemRenameHelperTests, RenameReplaceRefusesADirectoryDestination)
    {
        fileSystem_->addFile(remotePath("/home/user/destination.txt/inner.txt"), "inner");

        EXPECT_FALSE(renameReplace().has_value());
        EXPECT_EQ(fileSystem_->contentOf(source), "source");
        EXPECT_EQ(fileSystem_->contentOf(remotePath("/home/user/destination.txt/inner.txt")), "inner");
    }

    TEST_P(RemoteFileSystemRenameHelperTests, MoveIntoPlaceReplacesOnlyWhenAsked)
    {
        fileSystem_->addFile(destination, "destination");

        for (const bool replace : {false, true})
        {
            const auto moved = runInStrand(
                *fileSystem_,
                [this, replace]()
                {
                    return moveIntoPlaceInStrand(*fileSystem_, source, destination, replace, ".aside");
                }
            );
            EXPECT_EQ(moved.has_value(), replace);
            EXPECT_EQ(fileSystem_->contentOf(destination), replace ? "source" : "destination");
        }
    }

    class RemoteFileSystemReadHelperTests
        : public RemoteFileSystemHelperFixture
        , public ::testing::Test
    {
      protected:
        std::shared_ptr<FakeRemoteFileSystem> fileSystem_{makeFileSystem({.capabilities = {.userDirectories = true}})};
    };

    TEST_F(RemoteFileSystemReadHelperTests, ReadSmallFileReadsUpToTheMaximum)
    {
        std::string content;
        for (int line = 0; content.size() < 3000u; ++line)
            content += fmt::format("line {}\n", line);
        fileSystem_->addFile(remotePath("/home/user/notes.txt"), content);

        for (const std::size_t maximumSize : {std::size_t{100}, std::size_t{2048}, std::size_t{64 * 1024}})
        {
            const auto read = runInStrand(
                *fileSystem_,
                [this, maximumSize]()
                {
                    return readSmallFileInStrand(*fileSystem_, remotePath("/home/user/notes.txt"), maximumSize);
                }
            );
            ASSERT_TRUE(read.has_value()) << read.error().toString();
            EXPECT_EQ(*read, content.substr(0, maximumSize));
        }
        const auto missing = runInStrand(
            *fileSystem_,
            [this]()
            {
                return readSmallFileInStrand(*fileSystem_, remotePath("/home/user/missing.txt"), 100u);
            }
        );
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error().kind, RemoteErrorKind::NoSuchFile);
    }

    TEST_F(RemoteFileSystemReadHelperTests, UserDirectoriesAreReadFromTheHomeDirectory)
    {
        fileSystem_->addFile(
            remotePath("/home/user/.config/user-dirs.dirs"),
            "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\nXDG_MUSIC_DIR=\"$HOME/Musik\"\n"
        );

        const auto directories = runInStrand(
            *fileSystem_,
            [this]()
            {
                return userDirectoriesInStrand(*fileSystem_, remotePath("/home/user"));
            }
        );

        ASSERT_TRUE(directories.has_value()) << directories.error().toString();
        ASSERT_EQ(directories->size(), 2u);
        EXPECT_EQ(directories->at(0).name, "DOWNLOAD");
        EXPECT_EQ(directories->at(0).path, "/home/user/Downloads");
        EXPECT_EQ(directories->at(1).name, "MUSIC");
        EXPECT_EQ(directories->at(1).path, "/home/user/Musik");
    }

    TEST_F(RemoteFileSystemReadHelperTests, UserDirectoriesAreNoneWithoutTheFileOrTheCapability)
    {
        const auto withoutFile = runInStrand(
            *fileSystem_,
            [this]()
            {
                return userDirectoriesInStrand(*fileSystem_, remotePath("/home/user"));
            }
        );
        ASSERT_TRUE(withoutFile.has_value()) << withoutFile.error().toString();
        EXPECT_TRUE(withoutFile->empty());

        const auto incapable = makeFileSystem({});
        incapable->addFile(remotePath("/home/user/.config/user-dirs.dirs"), "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n");
        const auto withoutCapability = runInStrand(
            *incapable,
            [&incapable]()
            {
                return userDirectoriesInStrand(*incapable, remotePath("/home/user"));
            }
        );
        ASSERT_TRUE(withoutCapability.has_value()) << withoutCapability.error().toString();
        EXPECT_TRUE(withoutCapability->empty());
        EXPECT_EQ(incapable->started<OpenFile>(), 0u);
    }

    TEST_F(RemoteFileSystemReadHelperTests, UserDirectoriesRefuseAnUnsetHome)
    {
        fileSystem_->addFile(remotePath("/.config/user-dirs.dirs"), "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n");

        const auto refused = runInStrand(
            *fileSystem_,
            [this]()
            {
                return userDirectoriesInStrand(*fileSystem_, Paths::RemotePath{});
            }
        );

        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().wrapperError, WrapperErrors::InvalidPath);
        EXPECT_EQ(fileSystem_->started<OpenFile>(), 0u);
    }
}

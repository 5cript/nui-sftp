#pragma once

#include <backend/sftp/local_scan_operation.hpp>
#include "real_server_tests.hpp"

#include <paths/native.hpp>
#include <shared_data/sync/diff_tree_store.hpp>
#include <utility/temporary_directory.hpp>

#include <nui/utility/scope_exit.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

extern std::filesystem::path temporaryRoot;

#ifndef _WIN32
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace Test
{
    class LocalScanOperationTests : public ::testing::Test
    {
      public:
        void SetUp() override
        {
            writeFile(scanRoot_.path() / "file1.txt", "Local file content");
            writeFile(scanRoot_.path() / "notes.md", "hello");
            std::filesystem::create_directory(scanRoot_.path() / "subdir");
            writeFile(scanRoot_.path() / "subdir" / "nested.txt", "nested content");
            std::filesystem::create_directory(scanRoot_.path() / "subdir" / "deeper");
            writeFile(scanRoot_.path() / "subdir" / "deeper" / "leaf.txt", "leaf");

            // Hidden files — used by the ignoreHidden tests.
            writeFile(scanRoot_.path() / ".hidden_file", "secret");
            std::filesystem::create_directory(scanRoot_.path() / ".hidden_dir");
            writeFile(scanRoot_.path() / ".hidden_dir" / "child.txt", "child");
        }

      protected:
        static void writeFile(std::filesystem::path const& path, std::string_view contents)
        {
            std::ofstream stream{path, std::ios::binary};
            stream << contents;
        }

        static LocalScanOperation::WorkStatus
        runScanToCompletion(LocalScanOperation& operation, int maxIterations = 500)
        {
            LocalScanOperation::WorkStatus last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                auto workResult = operation.work();
                EXPECT_TRUE(workResult.has_value()) << "work() failed unexpectedly";
                if (!workResult.has_value())
                    return last;
                last = workResult.value();
                if (last == LocalScanOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        static bool hasEntryNamed(
            std::vector<SharedData::DirectoryEntry> const& entries,
            std::string const& filename)
        {
            return std::any_of(entries.begin(), entries.end(), [&](SharedData::DirectoryEntry const& entry) {
                return entry.name == filename;
            });
        }

        static SharedData::DirectoryEntry const* findEntryNamed(
            std::vector<SharedData::DirectoryEntry> const& entries,
            std::string const& filename)
        {
            auto iter = std::find_if(entries.begin(), entries.end(), [&](SharedData::DirectoryEntry const& entry) {
                return entry.name == filename;
            });
            return iter == entries.end() ? nullptr : &*iter;
        }

      protected:
        Utility::TemporaryDirectory scanRoot_{temporaryRoot, true};
    };

    TEST_F(LocalScanOperationTests, CanCreateLocalScanOperation)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path())}};
    }

    TEST_F(LocalScanOperationTests, LocalScanOperationTypeIsLocalScan)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path())}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::LocalScan);
    }

    TEST_F(LocalScanOperationTests, LocalScanOperationIsBarrier)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path())}};
        EXPECT_TRUE(operation.isBarrier());
    }

    TEST_F(LocalScanOperationTests, CanScanSingleDirectory)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "notes.md"));
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
    }

    TEST_F(LocalScanOperationTests, NonRecursiveScanDoesNotDescend)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
        EXPECT_FALSE(hasEntryNamed(entries, "nested.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, "leaf.txt"));
    }

    TEST_F(LocalScanOperationTests, RecursiveScanFindsNestedEntries)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
        EXPECT_TRUE(hasEntryNamed(entries, "nested.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "deeper"));
        EXPECT_TRUE(hasEntryNamed(entries, "leaf.txt"));
    }

    TEST_F(LocalScanOperationTests, ScanReportsCorrectFileSizeAndType)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();

        auto const* fileEntry = findEntryNamed(entries, "file1.txt");
        ASSERT_NE(fileEntry, nullptr);
        EXPECT_EQ(fileEntry->type, SharedData::FileType::Regular);
        EXPECT_EQ(fileEntry->size, std::string{"Local file content"}.size());

        auto const* dirEntry = findEntryNamed(entries, "subdir");
        ASSERT_NE(dirEntry, nullptr);
        EXPECT_EQ(dirEntry->type, SharedData::FileType::Directory);
        EXPECT_EQ(dirEntry->size, 0u);
    }

    TEST_F(LocalScanOperationTests, TotalBytesMatchesSumOfRegularFiles)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path() / "subdir"),
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto reportedBytes = operation.totalBytes();
        const auto expectedBytes =
            std::string{"nested content"}.size() + std::string{"leaf"}.size();
        EXPECT_EQ(reportedBytes, expectedBytes);
    }

    TEST_F(LocalScanOperationTests, ProgressCallbackInvokedAtLeastOnce)
    {
        int callbackInvocations = 0;
        LocalScanOperation operation{
            {
                .progressCallback =
                    [&callbackInvocations](std::uint64_t, std::uint64_t, std::uint64_t) {
                        ++callbackInvocations;
                    },
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(LocalScanOperationTests, IgnoreHiddenSkipsDotfiles)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
                .ignoreHidden = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, ".hidden_file"));
        EXPECT_FALSE(hasEntryNamed(entries, ".hidden_dir"));
    }

    TEST_F(LocalScanOperationTests, WithoutIgnoreHiddenDotfilesArePresent)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
                .ignoreHidden = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, ".hidden_file"));
        EXPECT_TRUE(hasEntryNamed(entries, ".hidden_dir"));
    }

    TEST_F(LocalScanOperationTests, ScanNonExistentLocalDirectoryFailsGracefully)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path() / "does_not_exist"),
                .recursive = false,
            }};

        // We want a graceful error result, not an uncaught filesystem exception.
        std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> workResult{
            LocalScanOperation::WorkStatus::MoreWork};
        bool threw = false;
        try
        {
            for (int iteration = 0; iteration < 100; ++iteration)
            {
                workResult = operation.work();
                if (!workResult.has_value())
                    break;
                if (workResult.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
        }
        catch (...)
        {
            threw = true;
        }

        EXPECT_FALSE(threw) << "scan of a non-existent directory must not throw";
        ASSERT_FALSE(workResult.has_value()) << "scan of a non-existent directory must return an error";
        EXPECT_EQ(workResult.error().type, LocalScanOperation::ErrorType::FilesystemError);
    }

    TEST_F(LocalScanOperationTests, ScanOfRegularFilePathFailsGracefully)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path() / "file1.txt"),
                .recursive = false,
            }};

        std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> workResult{
            LocalScanOperation::WorkStatus::MoreWork};
        bool threw = false;
        try
        {
            for (int iteration = 0; iteration < 100; ++iteration)
            {
                workResult = operation.work();
                if (!workResult.has_value())
                    break;
                if (workResult.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
        }
        catch (...)
        {
            threw = true;
        }

        EXPECT_FALSE(threw) << "scan of a regular file path must not throw";
        ASSERT_FALSE(workResult.has_value()) << "scan of a regular file path must return an error";
        EXPECT_EQ(workResult.error().type, LocalScanOperation::ErrorType::FilesystemError);
    }

    TEST_F(LocalScanOperationTests, WorkOnCompletedScanReturnsError)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, LocalScanOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(LocalScanOperationTests, WorkOnFailedScanReturnsError)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path() / "does_not_exist"),
                .recursive = false,
            }};

        try
        {
            for (int iteration = 0; iteration < 10; ++iteration)
            {
                auto result = operation.work();
                if (!result.has_value())
                    break;
            }
        }
        catch (...)
        {
            // Swallow here so the "work-after-failure" probe below still runs.
        }

        auto secondAttempt = operation.work();
        ASSERT_FALSE(secondAttempt.has_value());
        EXPECT_EQ(secondAttempt.error().type, LocalScanOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(LocalScanOperationTests, CancelledScanCannotBeWorkedFurther)
    {
        LocalScanOperation operation{
            {
                .localPath = toLocal(scanRoot_.path()),
                .recursive = true,
            }};

        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, LocalScanOperation::ErrorType::CannotWorkCanceledOperation);
    }

    TEST_F(LocalScanOperationTests, ScanStrandIsNullForLocalScan)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path())}};
        EXPECT_EQ(operation.strand(), nullptr);
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    TEST_F(LocalScanOperationTests, NamesThatAreNotUtf8AreShownButRejected)
    {
        const auto latin1 = scanRoot_.path() / std::filesystem::path{std::string{"Fran\xE7ois.txt"}};
        writeFile(latin1, "latin1");
        std::error_code error{};
        if (!std::filesystem::exists(latin1, error))
            GTEST_SKIP() << "The filesystem refuses names that are not UTF-8";

        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = false}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        const auto rejected = operation.rejectedPaths();
        ASSERT_EQ(rejected.size(), 1u);
        EXPECT_TRUE(Paths::isValidUtf8(rejected.front()));
        EXPECT_TRUE(rejected.front().ends_with("/Fran\xEF\xBF\xBDois.txt")) << rejected.front();
        const auto entries = operation.ejectEntries();
        EXPECT_FALSE(hasEntryNamed(entries, "Fran\xEF\xBF\xBDois.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));

        LocalScanOperation treeScan{{.localPath = toLocal(scanRoot_.path()), .recursive = false, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(treeScan), LocalScanOperation::WorkStatus::Complete);
        const auto tree = treeScan.ejectScanTree();
        auto const* node = SharedData::Sync::findScanNode(tree, "Fran\xEF\xBF\xBDois.txt");
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->availability, SharedData::Sync::Availability::InvalidName);
    }
#endif

    TEST_F(LocalScanOperationTests, BuildTreeNestsAndSortsEntries)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = true, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto tree = operation.ejectScanTree();
        std::vector<std::string> names;
        for (auto const& child : tree.children)
            names.push_back(child.name);
        EXPECT_EQ(names, (std::vector<std::string>{".hidden_dir", ".hidden_file", "file1.txt", "notes.md", "subdir"}));

        auto const& subdir = tree.children.back();
        EXPECT_TRUE(subdir.childrenKnown);
        EXPECT_EQ(subdir.subtreeFileCount, 2u);
        EXPECT_EQ(subdir.subtreeByteTotal, std::string_view{"nested content"}.size() + std::string_view{"leaf"}.size());
    }

    TEST_F(LocalScanOperationTests, BuildTreeExcludesTransferTempFiles)
    {
        std::ofstream{scanRoot_.path() / "big.iso.filepart"} << "partial";
        std::ofstream{scanRoot_.path() / ".filepart"} << "a name, not a temp file";

        LocalScanOperation operation{{
            .localPath = toLocal(scanRoot_.path()),
            .recursive = true,
            .buildTree = true,
            .excludedSuffixes = {".filepart"},
        }};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto tree = operation.ejectScanTree();
        auto const* temp = SharedData::Sync::findScanNode(tree, "big.iso.filepart");
        ASSERT_NE(temp, nullptr);
        EXPECT_EQ(temp->availability, SharedData::Sync::Availability::Excluded);
        auto const* bare = SharedData::Sync::findScanNode(tree, ".filepart");
        ASSERT_NE(bare, nullptr);
        EXPECT_EQ(bare->availability, SharedData::Sync::Availability::Listed);
    }

    TEST_F(LocalScanOperationTests, NonRecursiveBuildTreeMarksSubdirectoriesUnknown)
    {
        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = false, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto tree = operation.ejectScanTree();
        auto const subdir = std::find_if(
            tree.children.begin(),
            tree.children.end(),
            [](auto const& child)
            {
                return child.name == "subdir";
            }
        );
        ASSERT_NE(subdir, tree.children.end());
        EXPECT_FALSE(subdir->childrenKnown) << "the diff must not delete what the scan never listed";
        EXPECT_TRUE(subdir->children.empty());
    }

    TEST_F(LocalScanOperationTests, BuildTreeHonorsIgnoreFilesAndHidden)
    {
        writeFile(scanRoot_.path() / ".gitignore", "*.md\ndeeper/\n");
        LocalScanOperation operation{
            {.localPath = toLocal(scanRoot_.path()),
                .respectIgnoreFiles = true,
                .recursive = true,
                .ignoreHidden = true,
                .buildTree = true}
        };
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto tree = operation.ejectScanTree();
        std::vector<std::string> names;
        for (auto const& child : tree.children)
            names.push_back(child.name);
        EXPECT_EQ(names, (std::vector<std::string>{"file1.txt", "subdir"}));
        ASSERT_EQ(tree.children.back().children.size(), 1u);
        EXPECT_EQ(tree.children.back().children.front().name, "nested.txt");
    }

#ifndef _WIN32
    TEST_F(LocalScanOperationTests, BuildTreeKeepsUnreadableDirectoryAsUnreadable)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can list any directory";
        const auto locked = scanRoot_.path() / "subdir" / "deeper";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&locked]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = true, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto tree = operation.ejectScanTree();
        auto const* deeper = SharedData::Sync::findScanNode(tree, "subdir/deeper");
        ASSERT_NE(deeper, nullptr);
        EXPECT_EQ(deeper->availability, SharedData::Sync::Availability::Unreadable);
        EXPECT_FALSE(deeper->childrenKnown) << "unknown content must never look empty";
    }

    TEST_F(LocalScanOperationTests, FlatScanReportsAnUnreadableDirectory)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can list any directory";
        const auto locked = scanRoot_.path() / "subdir" / "deeper";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&locked]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        EXPECT_EQ(operation.unreadableDirectories(), (std::vector<std::string>{Paths::nativeToUtf8(locked)}));
    }

    TEST_F(LocalScanOperationTests, UnreadableRootFailsTheScan)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can list any directory";
        const auto locked = scanRoot_.path() / "subdir";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&locked]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        LocalScanOperation operation{{.localPath = toLocal(locked), .recursive = true, .buildTree = true}};
        std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> result{};
        for (int iteration = 0; iteration < 10 && result.has_value() &&
             result.value() != LocalScanOperation::WorkStatus::Complete;
             ++iteration)
            result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().localProblem, SharedData::LocalFileProblem::PermissionDenied);
    }

    TEST_F(LocalScanOperationTests, BuildTreeKeepsFifoAsExcludedPlaceholder)
    {
        const auto fifo = scanRoot_.path() / "pipe";
        ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = true, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        const auto tree = operation.ejectScanTree();
        auto const* pipe = SharedData::Sync::findScanNode(tree, "pipe");
        ASSERT_NE(pipe, nullptr);
        EXPECT_EQ(pipe->availability, SharedData::Sync::Availability::Excluded);

        LocalScanOperation flat{{.localPath = toLocal(scanRoot_.path()), .recursive = true}};
        ASSERT_EQ(runScanToCompletion(flat), LocalScanOperation::WorkStatus::Complete);
        EXPECT_FALSE(hasEntryNamed(flat.ejectEntries(), "pipe")) << "transfers never see untransferable entries";
    }
#endif

#ifdef _WIN32
    TEST_F(LocalScanOperationTests, JunctionIsExcludedAndNotWalked)
    {
        const auto outside = scanRoot_.path().parent_path() / ("junction_target_" + scanRoot_.path().filename().string());
        std::filesystem::create_directories(outside);
        std::ofstream{outside / "inside.txt"} << "inside";
        auto removeOutside = Nui::ScopeExit{[&outside]() noexcept {
            std::error_code errorCode{};
            std::filesystem::remove_all(outside, errorCode);
        }};
        const auto link = scanRoot_.path() / "linked";
        const auto command = "cmd /c mklink /J \"" + link.string() + "\" \"" + outside.string() + "\" >nul";
        ASSERT_EQ(std::system(command.c_str()), 0);

        LocalScanOperation operation{{.localPath = toLocal(scanRoot_.path()), .recursive = true, .buildTree = true}};
        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        const auto tree = operation.ejectScanTree();
        auto const* linked = SharedData::Sync::findScanNode(tree, "linked");
        ASSERT_NE(linked, nullptr);
        EXPECT_EQ(linked->availability, SharedData::Sync::Availability::Excluded);
        EXPECT_EQ(SharedData::Sync::findScanNode(tree, "linked/inside.txt"), nullptr) << "a junction is never walked";

        LocalScanOperation flat{{.localPath = toLocal(scanRoot_.path()), .recursive = true}};
        ASSERT_EQ(runScanToCompletion(flat), LocalScanOperation::WorkStatus::Complete);
        const auto entries = flat.ejectEntries();
        EXPECT_FALSE(hasEntryNamed(entries, "linked"));
        EXPECT_FALSE(hasEntryNamed(entries, "inside.txt")) << "an upload never leaves its tree through a junction";
    }
#endif
}

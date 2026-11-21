#pragma once

#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/delete_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include "operation_runs.hpp"
#include "remote_protocol_tests.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace Test
{
    /**
     * @brief The checks that keep a sync from replacing or deleting what changed after its compare, with the states
     *        a scan saw, on both protocols: an FTP listing tells times only to the minute.
     */
    class ProtocolSyncCheckTests : public RemoteProtocolTests
    {
      protected:
        /**
         * @brief What a scan of the work directory saw, by name.
         */
        std::map<std::string, SharedData::ComparedState> scannedStates()
        {
            ScanOperation scan{fileSystem(), {.remotePath = remote(std::string{workDirectory}), .recursive = false}};
            const auto result = runToEnd(scan, operationLimit);
            EXPECT_TRUE(result.has_value()) << result.error().toString();
            std::map<std::string, SharedData::ComparedState> states{};
            for (auto const& entry : scan.ejectEntries())
            {
                if (entry.parent)
                    states.emplace(entry.name, SharedData::comparedStateOf(entry));
            }
            return states;
        }

        std::expected<void, Operation::Error>
        checkRemote(std::string_view relative, SharedData::ComparedState const& compared)
        {
            auto checking = fileSystem()->performPromise(
                [fileSystem = fileSystem(), path = remoteAt(relative), compared]()
                {
                    return ComparedStateCheck::requireUnchangedRemoteInStrand(*fileSystem, path, compared);
                }
            );
            return checking.get();
        }

        UploadOperation::UploadOperationOptions
        replacingUploadOf(std::string_view name, std::string_view content, SharedData::ComparedState compared) const
        {
            const auto local = localDirectory_.path() / name;
            writeLocalFile(local, content);
            return {
                .remotePath = remoteAt(name),
                .localPath = toLocal(local),
                .mayOverwrite = true,
                .comparedDestination = std::move(compared),
            };
        }
    };

    TEST_P(ProtocolSyncCheckTests, TheRemoteCheckPassesOnlyWhatTheScanSaw)
    {
        putRemoteFile("file.txt", "content");
        putRemoteDirectory("directory");
        const auto states = scannedStates();
        ASSERT_TRUE(states.contains("file.txt"));
        ASSERT_TRUE(states.contains("directory"));

        EXPECT_TRUE(checkRemote("file.txt", states.at("file.txt")).has_value()) << "unchanged";
        EXPECT_TRUE(checkRemote("directory", states.at("directory")).has_value()) << "unchanged";
        EXPECT_TRUE(checkRemote("missing.txt", SharedData::ComparedState{}).has_value()) << "still absent";

        putRemoteFile("file.txt", "changed content");
        const auto changed = checkRemote("file.txt", states.at("file.txt"));
        ASSERT_FALSE(changed.has_value());
        EXPECT_EQ(changed.error().type, Operation::ErrorType::ChangedSinceCompare);

        putRemoteFile("appeared.txt", "new");
        const auto appeared = checkRemote("appeared.txt", SharedData::ComparedState{});
        ASSERT_FALSE(appeared.has_value());
        EXPECT_EQ(appeared.error().type, Operation::ErrorType::ChangedSinceCompare);

        const auto replaced = checkRemote("directory", states.at("file.txt"));
        ASSERT_FALSE(replaced.has_value());
        EXPECT_EQ(replaced.error().type, Operation::ErrorType::ChangedSinceCompare);
    }

    TEST_P(ProtocolSyncCheckTests, AnUploadReplacesADestinationThatIsAsTheScanSawIt)
    {
        putRemoteFile("file.txt", "content");
        const auto states = scannedStates();
        ASSERT_TRUE(states.contains("file.txt"));
        UploadOperation operation{fileSystem(), replacingUploadOf("file.txt", "new content", states.at("file.txt"))};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("file.txt"), "new content");
    }

    TEST_P(ProtocolSyncCheckTests, AnUploadNeverReplacesADestinationThatChangedAfterTheScan)
    {
        putRemoteFile("file.txt", "content");
        const auto states = scannedStates();
        ASSERT_TRUE(states.contains("file.txt"));
        putRemoteFile("file.txt", "edited content");
        UploadOperation operation{fileSystem(), replacingUploadOf("file.txt", "new content", states.at("file.txt"))};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::ChangedSinceCompare);
        EXPECT_EQ(remoteContentOf("file.txt"), "edited content");
        EXPECT_FALSE(remoteExists("file.txt.filepart"));
    }

    TEST_P(ProtocolSyncCheckTests, ADeleteLeavesATargetThatChangedAfterTheScan)
    {
        putRemoteFile("changed.txt", "a");
        putRemoteFile("unchanged.txt", "b");
        const auto states = scannedStates();
        ASSERT_TRUE(states.contains("changed.txt"));
        ASSERT_TRUE(states.contains("unchanged.txt"));
        putRemoteFile("changed.txt", "edited");

        DeleteOperation operation{fileSystem(), {.recursive = true}};
        operation.setTargets({
            {.path = remoteAt("changed.txt"), .compared = states.at("changed.txt")},
            {.path = remoteAt("unchanged.txt"), .compared = states.at("unchanged.txt")},
        });
        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("changed.txt"), "edited");
        EXPECT_FALSE(remoteExists("unchanged.txt"));
        ASSERT_EQ(operation.getFailed().size(), 1u);
        EXPECT_EQ(operation.getFailed().front().second.type, Operation::ErrorType::ChangedSinceCompare);
    }

    INSTANTIATE_FOR_BOTH_PROTOCOLS(ProtocolSyncCheckTests);
}

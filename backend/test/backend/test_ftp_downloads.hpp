#pragma once

#include <backend/sftp/download_operation.hpp>
#include <ftp/support/tree_responder.hpp>
#include <utility/permissions.hpp>
#include <utility/temporary_directory.hpp>
#include "ftp_served_tree_tests.hpp"
#include "operation_runs.hpp"
#include "real_server_tests.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class FtpDownloadTests : public FtpServedTreeTests
    {};

#ifndef _WIN32
    TEST_F(FtpDownloadTests, ADownloadInheritsThePermissionsTheListingShowed)
    {
        using FileTransferProtocol::Test::servedDirectory;
        using FileTransferProtocol::Test::servedFile;
        auto script = servedFile("run.sh", "#!/bin/sh\n");
        script.permissions = "rwxr-x---";
        serve({servedDirectory("tree", {std::move(script)})});
        auto listed = fileSystem()->later(RemoteIo::Lstat{.path = remote("/tree/run.sh")}).get();
        ASSERT_TRUE(listed) << listed.error().toString();
        using std::filesystem::perms;
        const auto shown = perms::owner_all | perms::group_read | perms::group_exec;
        ASSERT_EQ(listed->permissions, shown);

        // SIZE and MDTM, which a stream of FTP asks, tell no permissions.
        const Utility::TemporaryDirectory run{temporaryRoot, true};
        const auto localPath = run.path() / "run.sh";
        DownloadOperation operation{
            fileSystem(),
            {
                .remotePath = remote("/tree/run.sh"),
                .localPath = toLocal(localPath),
                .inheritPermissions = true,
                .bigFileOptimized = true,
                .entry = *listed,
                .markAsDownloaded = false,
            }
        };
        const auto result = runToEnd(operation, operationLimit);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(std::filesystem::status(localPath).permissions(), shown & ~Utility::processUmask());
    }
#endif
}

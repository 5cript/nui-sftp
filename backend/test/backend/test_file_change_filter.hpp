#pragma once

#include <backend/file_tracking/change_filter.hpp>
#include <persistence/state/sftp_options.hpp>

#include <gtest/gtest.h>

namespace Test
{
    using FileTracking::FileAction;
    using FileTracking::FileChange;
    using FileTracking::filterFileChange;
    using FileTracking::isTransientFile;

    constexpr char const* filepart = ".filepart";

    TEST(FileChangeFilterTests, TemporariesOfEditorsFileManagersAndDownloadsAreTransient)
    {
        for (
            auto const* name : {
                ".goutputstream-ABC123",
                "report.pdf.filepart",
                ".DS_Store",
                "._report.pdf",
                "notes.txt.sb-b5400103-2RCJIT",
                "Public/notes.txt.sb-0123abcd-x9Y8z7",
            }
        )
            EXPECT_TRUE(isTransientFile(name, filepart)) << name;
    }

    TEST(FileChangeFilterTests, OrdinaryFilesAreNotTransient)
    {
        for (
            auto const* name : {
                "notes.txt",
                "my.sb-config",
                "archive.sb-12345678-short",
                "DS_Store",
                "filepart.txt",
            }
        )
            EXPECT_FALSE(isTransientFile(name, filepart)) << name;
    }

    TEST(FileChangeFilterTests, ChangesOfTransientFilesAreDropped)
    {
        EXPECT_FALSE(filterFileChange({FileAction::Modified, ".DS_Store"}, false, filepart).has_value());
        EXPECT_FALSE(
            filterFileChange({FileAction::Deleted, "bla.txt.sb-b5400103-2RCJIT"}, false, filepart).has_value()
        );
        EXPECT_FALSE(filterFileChange({FileAction::Added, "readme.txt.filepart"}, false, filepart).has_value());
    }

    TEST(FileChangeFilterTests, FinishedDownloadIsNotMirroredAsARename)
    {
        // The two failed remote renames on macOS: FSEvents reported the downloads' own renames.
        EXPECT_FALSE(
            filterFileChange({FileAction::Moved, "readme.txt", "readme.txt.filepart"}, false, filepart).has_value()
        );
    }

    TEST(FileChangeFilterTests, AtomicSaveBecomesAModificationOfTheRealFile)
    {
        const auto change =
            filterFileChange({FileAction::Moved, "notes.txt", ".goutputstream-ABC123"}, false, filepart);
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(change->action, FileAction::Modified);
        EXPECT_EQ(change->filename, "notes.txt");
        EXPECT_TRUE(change->oldFilename.empty());
    }

    TEST(FileChangeFilterTests, ModifiedDirectoryIsDropped)
    {
        EXPECT_FALSE(filterFileChange({FileAction::Modified, "Public"}, true, filepart).has_value());
        EXPECT_TRUE(filterFileChange({FileAction::Added, "Public/new"}, true, filepart).has_value());
    }

    TEST(FileChangeFilterTests, ConfiguredDownloadSuffixIsHonoured)
    {
        EXPECT_TRUE(isTransientFile("readme.txt.part", ".part"));
        EXPECT_FALSE(isTransientFile("readme.txt.filepart", ".part"));
        EXPECT_FALSE(isTransientFile(".part", ".part"));
        EXPECT_FALSE(
            filterFileChange({FileAction::Moved, "readme.txt", "readme.txt.part"}, false, ".part").has_value()
        );
        // Another suffix renamed over the real file is an editor's save under the configured one.
        const auto change = filterFileChange({FileAction::Moved, "readme.txt", "readme.txt.filepart"}, false, ".part");
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(change->action, FileAction::Moved);
    }

    TEST(FileChangeFilterTests, EffectiveTempFileSuffixKeepsAConfiguredSuffix)
    {
        // The transfers used to replace every suffix without a leading slash by .filepart.
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(".part"), ".part");
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(".nui-sftp~"), ".nui-sftp~");
    }

    TEST(FileChangeFilterTests, EffectiveTempFileSuffixFallsBackWhereTheConfiguredOneIsUnusable)
    {
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(std::nullopt), ".filepart");
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(""), ".filepart");
        EXPECT_EQ(Persistence::effectiveTempFileSuffix("/part"), ".filepart");
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(".a/b"), ".filepart");
        EXPECT_EQ(Persistence::effectiveTempFileSuffix(".a\\b"), ".filepart");
    }

    TEST(FileChangeFilterTests, RealRenamePassesThrough)
    {
        const auto change = filterFileChange({FileAction::Moved, "new.txt", "old.txt"}, false, filepart);
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(change->action, FileAction::Moved);
        EXPECT_EQ(change->oldFilename, "old.txt");
    }
}

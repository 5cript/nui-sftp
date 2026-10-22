#pragma once

#include <shared_data/file_operations/compared_state.hpp>

#include <gtest/gtest.h>

namespace SharedData::Test
{
    namespace ComparedStateTestDetail
    {
        inline ComparedState file(std::uint64_t size, std::uint64_t mtime)
        {
            return ComparedState{.exists = true, .type = FileType::Regular, .size = size, .mtime = mtime};
        }

        inline ComparedState link(std::string target)
        {
            return ComparedState{.exists = true, .type = FileType::Symlink, .mtime = 5, .linkTarget = std::move(target)};
        }
    }

    TEST(ComparedStateTests, AnUnchangedFilePasses)
    {
        using namespace ComparedStateTestDetail;
        EXPECT_TRUE(unchangedSinceCompare(file(3, 100), file(3, 100)));
    }

    TEST(ComparedStateTests, AFileWithAnotherSizeOrTimeChanged)
    {
        using namespace ComparedStateTestDetail;
        EXPECT_FALSE(unchangedSinceCompare(file(3, 100), file(4, 100)));
        EXPECT_FALSE(unchangedSinceCompare(file(3, 100), file(3, 101)));
    }

    TEST(ComparedStateTests, SomethingThatAppearedWhereNothingWasChanged)
    {
        using namespace ComparedStateTestDetail;
        EXPECT_TRUE(unchangedSinceCompare(ComparedState{}, ComparedState{}));
        EXPECT_FALSE(unchangedSinceCompare(ComparedState{}, file(3, 100)));
        EXPECT_FALSE(unchangedSinceCompare(ComparedState{}, ComparedState{.exists = true, .type = FileType::Directory}));
    }

    TEST(ComparedStateTests, SomethingThatIsGoneLosesNothing)
    {
        using namespace ComparedStateTestDetail;
        EXPECT_TRUE(unchangedSinceCompare(file(3, 100), ComparedState{}));
    }

    TEST(ComparedStateTests, SomethingThatMustStillExistChangedWhenItIsGone)
    {
        using namespace ComparedStateTestDetail;
        auto compared = file(3, 100);
        compared.mustStillExist = true;
        EXPECT_FALSE(unchangedSinceCompare(compared, ComparedState{}));
        EXPECT_TRUE(unchangedSinceCompare(compared, file(3, 100)));
        EXPECT_FALSE(unchangedSinceCompare(compared, file(4, 100)));
    }

    TEST(ComparedStateTests, AnotherTypeChanged)
    {
        using namespace ComparedStateTestDetail;
        EXPECT_FALSE(unchangedSinceCompare(file(0, 5), link("target")));
        EXPECT_FALSE(unchangedSinceCompare(ComparedState{.exists = true, .type = FileType::Directory}, file(0, 0)));
    }

    TEST(ComparedStateTests, ASymlinkChangesByItsLiteralOnly)
    {
        using namespace ComparedStateTestDetail;
        auto later = link("target");
        later.mtime = 99;
        EXPECT_TRUE(unchangedSinceCompare(link("target"), later));
        EXPECT_FALSE(unchangedSinceCompare(link("target"), link("other")));
    }

    TEST(ComparedStateTests, ADirectoryChangesByItsTypeOnly)
    {
        const ComparedState compared{.exists = true, .type = FileType::Directory, .size = 4096, .mtime = 1};
        const ComparedState current{.exists = true, .type = FileType::Directory, .size = 8192, .mtime = 2};
        EXPECT_TRUE(unchangedSinceCompare(compared, current));
    }

    TEST(ComparedStateTests, AnEntryDescribesItsState)
    {
        const DirectoryEntry entry{.type = FileType::Symlink, .size = 7, .mtime = 9, .linkTarget = "a/b"};
        EXPECT_EQ(
            comparedStateOf(entry),
            (ComparedState{.exists = true, .type = FileType::Symlink, .size = 7, .mtime = 9, .linkTarget = "a/b"})
        );
        const DirectoryEntry regular{.type = FileType::Regular, .size = 7, .mtime = 9, .linkTarget = "ignored"};
        EXPECT_FALSE(comparedStateOf(regular).linkTarget);
    }
}

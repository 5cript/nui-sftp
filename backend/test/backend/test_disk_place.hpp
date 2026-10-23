#pragma once

#include <backend/file_tracking/disk_place.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifdef __APPLE__
#    include <fcntl.h>
#    include <unistd.h>
#endif

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class DiskPlaceTest : public ::testing::Test
    {
      protected:
        std::filesystem::path root_;

        void SetUp() override
        {
            root_ = std::filesystem::canonical(temporaryRoot) /
                ("disk_place_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root_ / "watched" / "project" / "src");
            std::filesystem::create_directories(root_ / "trash");
            std::ofstream{root_ / "watched" / "project" / "src" / "main.cpp"} << "int main() {}";
        }

        void TearDown() override
        {
            std::filesystem::remove_all(root_);
        }

        FileTracking::DiskIdentity projectIdentity() const
        {
            return FileTracking::diskIdentityOf(root_ / "watched" / "project").value();
        }

        std::optional<bool> isDeleted(FileTracking::DiskIdentity const& identity) const
        {
            return FileTracking::isDeletedFromItsVolume(identity, root_ / "watched");
        }
    };

    TEST_F(DiskPlaceTest, ADirectoryMovedElsewhereOnItsVolumeIsNotDeleted)
    {
        const auto identity = projectIdentity();

        std::filesystem::rename(root_ / "watched" / "project", root_ / "trash" / "project");

#ifdef __APPLE__
        EXPECT_EQ(isDeleted(identity), false);
#else
        EXPECT_EQ(isDeleted(identity), std::nullopt);
#endif
    }

    TEST_F(DiskPlaceTest, ADirectoryRemovedWithItsContentIsDeletedOnlyWhereItsIdentityCanBeLookedUp)
    {
        const auto identity = projectIdentity();

        std::filesystem::remove_all(root_ / "watched" / "project");

#ifdef __APPLE__
        EXPECT_EQ(isDeleted(identity), true);
#else
        EXPECT_EQ(isDeleted(identity), std::nullopt);
#endif
    }

    TEST_F(DiskPlaceTest, AnIdentityOfAnotherVolumeCannotBeTold)
    {
        auto identity = projectIdentity();
        ++identity.device;

        std::filesystem::remove_all(root_ / "watched" / "project");

        EXPECT_EQ(isDeleted(identity), std::nullopt);
    }

#ifdef __APPLE__
    TEST_F(DiskPlaceTest, ADirectoryDeletedWhileItWasOpenIsDeletedOnceClosed)
    {
        const auto identity = projectIdentity();
        const int descriptor = ::open((root_ / "watched" / "project").c_str(), O_RDONLY | O_DIRECTORY);
        ASSERT_GE(descriptor, 0);

        std::filesystem::remove_all(root_ / "watched" / "project");
        EXPECT_EQ(isDeleted(identity), true);
        ::close(descriptor);

        EXPECT_EQ(isDeleted(identity), true);
    }
#endif
}

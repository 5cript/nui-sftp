#pragma once

#include <persistence/state/ui_options.hpp>

#include <gtest/gtest.h>

namespace Test
{
    TEST(UiOptionsTests, OutdatedDefaultIconPathsPointIntoTheIconBundle)
    {
        Persistence::UiOptions options{};
        options.fileGridExtensionIcons = {
            {".cpp", "icons/cpp.png"},
            {".py", "icons/Development/python.png"},
            {".js", "icons/Development/noun-js-4921450.png"},
        };

        EXPECT_TRUE(Persistence::updateOutdatedExtensionIconPaths(options));
        EXPECT_EQ(options.fileGridExtensionIcons.at(".cpp"), "icons/masks/Development/noun-c-4921443.png");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".py"), "icons/masks/Development/noun-python-1375869.png");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".js"), "icons/masks/Development/noun-js-4921450.png");
    }

    TEST(UiOptionsTests, CurrentAndCustomIconPathsAreKept)
    {
        Persistence::UiOptions options{};
        const auto defaults = options.fileGridExtensionIcons;
        options.fileGridExtensionIcons[".md"] = "icons/masks/Office/custom.png";

        EXPECT_FALSE(Persistence::updateOutdatedExtensionIconPaths(options));
        EXPECT_EQ(options.fileGridExtensionIcons.at(".md"), "icons/masks/Office/custom.png");
        options.fileGridExtensionIcons.erase(".md");
        EXPECT_EQ(options.fileGridExtensionIcons, defaults);
    }
}

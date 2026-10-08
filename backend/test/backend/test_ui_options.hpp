#pragma once

#include <persistence/state/ui_options.hpp>

#include <gtest/gtest.h>

namespace Test
{
    TEST(UiOptionsTests, OutdatedDefaultIconPathsPointAtTheShippedIcons)
    {
        Persistence::UiOptions options{};
        options.fileGridExtensionIcons = {
            {".cpp", "icons/cpp.png"},
            {".py", "icons/Development/python.png"},
            {".js", "icons/masks/Development/noun-js-4921450.png"},
            {".log", "icons/masks/Development/noun-log-4921382.png"},
        };

        EXPECT_TRUE(Persistence::updateOutdatedExtensionIconPaths(options));
        EXPECT_EQ(options.fileGridExtensionIcons.at(".cpp"), "icons/file-types/cplusplus.svg");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".py"), "icons/file-types/python.svg");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".js"), "icons/file-types/javascript.svg");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".log"), "icons/file.svg");
    }

    TEST(UiOptionsTests, OtherIconsOfTheRemovedBundleFallBackToTheGenericFileIcon)
    {
        Persistence::UiOptions options{};
        options.fileGridExtensionIcons = {
            {".md", "icons/masks/Office/noun-document.png"},
            {".blend", "icons/Applications/blender-logo.png"},
        };

        EXPECT_TRUE(Persistence::updateOutdatedExtensionIconPaths(options));
        EXPECT_EQ(options.fileGridExtensionIcons.at(".md"), "icons/file.svg");
        EXPECT_EQ(options.fileGridExtensionIcons.at(".blend"), "icons/Applications/blender-logo.png");
    }

    TEST(UiOptionsTests, CurrentAndCustomIconPathsAreKept)
    {
        Persistence::UiOptions options{};
        const auto defaults = options.fileGridExtensionIcons;
        options.fileGridExtensionIcons[".md"] = "icons/custom/markdown.svg";

        EXPECT_FALSE(Persistence::updateOutdatedExtensionIconPaths(options));
        EXPECT_EQ(options.fileGridExtensionIcons.at(".md"), "icons/custom/markdown.svg");
        options.fileGridExtensionIcons.erase(".md");
        EXPECT_EQ(options.fileGridExtensionIcons, defaults);
    }
}

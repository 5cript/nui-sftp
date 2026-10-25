#pragma once

#include <log/log.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string_view>

namespace Test::LoggerCases
{
    /**
     * @brief The latest message @p message the global logger keeps for the frontend, none when it keeps none such.
     */
    inline std::optional<Log::Logger::StashedMessage> stashedGlobally(std::string_view message)
    {
        const auto stashed = Log::Detail::logger.stashed();
        const auto found = std::find_if(
            stashed.rbegin(),
            stashed.rend(),
            [message](Log::Logger::StashedMessage const& stashedMessage)
            {
                return stashedMessage.message == message;
            }
        );
        if (found == stashed.rend())
            return std::nullopt;
        return *found;
    }

    TEST(LoggerTest, AMessageStashedBeforeTheFrontendIsReadyKeepsWhetherItSkipsTheToast)
    {
        Log::Logger logger{};

        logger.log(Log::Level::Error, Log::Toast::Skip, "Listed in a panel {}", 1);
        logger.log(Log::Level::Error, "Shown as a toast {}", 2);

        const auto stashed = logger.stashed();
        ASSERT_EQ(stashed.size(), 2u);
        EXPECT_EQ(stashed[0].level, Log::Level::Error);
        EXPECT_EQ(stashed[0].toast, Log::Toast::Skip);
        EXPECT_EQ(stashed[0].message, "Listed in a panel 1");
        EXPECT_EQ(stashed[1].toast, Log::Toast::Show);
        EXPECT_EQ(stashed[1].message, "Shown as a toast 2");
    }

    TEST(LoggerTest, TheConvenienceFunctionsHandTheToastOn)
    {
        Log::error(Log::Toast::Skip, "Skipped error {}", 3);
        Log::warn(Log::Toast::Skip, "Skipped warning");
        Log::critical(Log::Toast::Skip, "Skipped critical");
        Log::log(Log::Level::Error, Log::Toast::Skip, "Skipped at a level of its own");
        Log::error("Shown error");

        const auto error = stashedGlobally("Skipped error 3");
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->level, Log::Level::Error);
        EXPECT_EQ(error->toast, Log::Toast::Skip);

        const auto warning = stashedGlobally("Skipped warning");
        ASSERT_TRUE(warning.has_value());
        EXPECT_EQ(warning->level, Log::Level::Warning);
        EXPECT_EQ(warning->toast, Log::Toast::Skip);

        const auto critical = stashedGlobally("Skipped critical");
        ASSERT_TRUE(critical.has_value());
        EXPECT_EQ(critical->level, Log::Level::Critical);
        EXPECT_EQ(critical->toast, Log::Toast::Skip);

        const auto atLevel = stashedGlobally("Skipped at a level of its own");
        ASSERT_TRUE(atLevel.has_value());
        EXPECT_EQ(atLevel->toast, Log::Toast::Skip);

        const auto shown = stashedGlobally("Shown error");
        ASSERT_TRUE(shown.has_value());
        EXPECT_EQ(shown->toast, Log::Toast::Show);
    }
}

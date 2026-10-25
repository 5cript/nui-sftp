#pragma once

#include <frontend/session_components/file_tracking/in_flight_rows.hpp>
#include <paths/paths.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking::Test
{
    namespace InFlightRowsTestDetail
    {
        using Rows = std::vector<std::optional<std::string>>;
    }

    TEST(InFlightRowsTests, NewKeysAppendRowsInTheirOrder)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt", "second.txt", "third.txt"}));
        EXPECT_EQ(rows.size(), std::size_t{3});
        EXPECT_EQ(rows.rowOf("first.txt"), std::optional<std::size_t>{0});
        EXPECT_EQ(rows.rowOf("second.txt"), std::optional<std::size_t>{1});
        EXPECT_EQ(rows.rowOf("third.txt"), std::optional<std::size_t>{2});
    }

    TEST(InFlightRowsTests, AKeyKeepsItsRowWhileItStaysInFlight)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"third.txt", "first.txt", "second.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt", "second.txt", "third.txt"}));
    }

    TEST(InFlightRowsTests, AFinishedKeyFreesItsRowWithoutShiftingTheOthers)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"first.txt", "third.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt", std::nullopt, "third.txt"}));
        EXPECT_EQ(rows.rowOf("second.txt"), std::nullopt);
        EXPECT_EQ(rows.rowOf("third.txt"), std::optional<std::size_t>{2});
    }

    TEST(InFlightRowsTests, ANewKeyReusesTheFirstFreeRow)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"first.txt", "third.txt"});
        rows.update({"first.txt", "third.txt", "fourth.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt", "fourth.txt", "third.txt"}));
    }

    TEST(InFlightRowsTests, NewKeysFillFreeRowsInOrderBeforeAppending)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt", "fourth.txt"});
        rows.update({"second.txt", "fourth.txt"});

        EXPECT_EQ(rows.rows(), (Rows{std::nullopt, "second.txt", std::nullopt, "fourth.txt"}));

        rows.update({"second.txt", "fourth.txt", "fifth.txt", "sixth.txt", "seventh.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"fifth.txt", "second.txt", "sixth.txt", "fourth.txt", "seventh.txt"}));
    }

    TEST(InFlightRowsTests, FreeRowsAtTheEndAreDropped)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"first.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt"}));
        EXPECT_EQ(rows.size(), std::size_t{1});
    }

    TEST(InFlightRowsTests, FreeRowsBeforeATakenRowStay)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"second.txt"});

        EXPECT_EQ(rows.rows(), (Rows{std::nullopt, "second.txt"}));
        EXPECT_EQ(rows.rowOf("second.txt"), std::optional<std::size_t>{1});
    }

    TEST(InFlightRowsTests, NothingInFlightLeavesNoRows)
    {
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt"});
        rows.update({});

        EXPECT_TRUE(rows.rows().empty());
        EXPECT_EQ(rows.size(), std::size_t{0});
        EXPECT_EQ(rows.rowOf("first.txt"), std::nullopt);
    }

    TEST(InFlightRowsTests, ARepeatedKeyTakesOneRow)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "first.txt", "second.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"first.txt", "second.txt"}));
    }

    TEST(InFlightRowsTests, AKeyThatReturnsTakesAFreeRowAgain)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt", "third.txt"});
        rows.update({"third.txt"});
        rows.update({"third.txt", "second.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"second.txt", std::nullopt, "third.txt"}));
    }

    TEST(InFlightRowsTests, ClearDropsEveryRow)
    {
        using namespace InFlightRowsTestDetail;
        InFlightRows<std::string> rows;
        rows.update({"first.txt", "second.txt"});
        rows.clear();

        EXPECT_TRUE(rows.rows().empty());

        rows.update({"second.txt"});

        EXPECT_EQ(rows.rows(), (Rows{"second.txt"}));
    }

    TEST(InFlightRowsTests, LocalPathsCanBeKeys)
    {
        const auto first = *Paths::LocalPath::fromUtf8("/home/user/project/first.txt", Paths::LocalPlatform::Posix);
        const auto second = *Paths::LocalPath::fromUtf8("/home/user/project/second.txt", Paths::LocalPlatform::Posix);
        const auto third = *Paths::LocalPath::fromUtf8("/home/user/project/third.txt", Paths::LocalPlatform::Posix);
        InFlightRows<Paths::LocalPath> rows;
        rows.update({first, second});
        rows.update({second, third});

        EXPECT_EQ(rows.rowOf(third), std::optional<std::size_t>{0});
        EXPECT_EQ(rows.rowOf(second), std::optional<std::size_t>{1});
        EXPECT_EQ(rows.rowOf(first), std::nullopt);
        EXPECT_EQ(rows.size(), std::size_t{2});
    }
}

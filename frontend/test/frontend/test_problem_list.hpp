#pragma once

#include <frontend/session_components/file_tracking/problem_list.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking::Test
{
    namespace ProblemListTestDetail
    {
        using Names = std::vector<std::string>;
        using List = ProblemList<std::string, std::string>;

        /**
         * @brief Shows each of @p names under its own name with the value "<name>-value", oldest first.
         */
        inline void showEach(List& list, Names const& names, std::uint64_t& sequence)
        {
            for (auto const& name : names)
                list.show(name, name + "-value", ++sequence);
        }

        inline Names idsOf(List const& list)
        {
            Names ids{};
            for (auto const& problem : list)
                ids.push_back(problem.id);
            return ids;
        }
    }

    TEST(ProblemListTests, AnEmptyListHasNothing)
    {
        using namespace ProblemListTestDetail;
        const List list{};

        EXPECT_TRUE(list.empty());
        EXPECT_EQ(list.size(), std::size_t{0});
        EXPECT_EQ(list.newest(), nullptr);
        EXPECT_EQ(list.find("unknown"), nullptr);
        EXPECT_FALSE(list.contains("unknown"));
        EXPECT_EQ(list.rankOf("unknown"), std::nullopt);
        EXPECT_EQ(list.slice(0u, 100u), Names{});
    }

    TEST(ProblemListTests, NewProblemsGoToTheFront)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third"}, sequence);

        EXPECT_EQ(idsOf(list), (Names{"third", "second", "first"}));
        EXPECT_EQ(list.size(), std::size_t{3});
        ASSERT_NE(list.newest(), nullptr);
        EXPECT_EQ(list.newest()->id, "third");
        EXPECT_EQ(list.newest()->sequence, 3u);
        EXPECT_EQ(list.slice(0u, 100u), (Names{"third-value", "second-value", "first-value"}));
    }

    TEST(ProblemListTests, ShowingAKnownIdAgainReplacesItAndMovesItToTheFront)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third"}, sequence);

        EXPECT_FALSE(list.show("first", "first-again", ++sequence));

        EXPECT_EQ(idsOf(list), (Names{"first", "third", "second"}));
        EXPECT_EQ(list.size(), std::size_t{3});
        ASSERT_NE(list.find("first"), nullptr);
        EXPECT_EQ(*list.find("first"), "first-again");
        EXPECT_EQ(list.rankOf("first"), std::optional<std::size_t>{0});
    }

    TEST(ProblemListTests, ShowTellsWhetherTheIdWasNew)
    {
        using namespace ProblemListTestDetail;
        List list{};

        EXPECT_TRUE(list.show("problem", "value", 1u));
        EXPECT_FALSE(list.show("problem", "value", 2u));
        EXPECT_EQ(list.size(), std::size_t{1});
    }

    TEST(ProblemListTests, AnOlderSequenceIsSortedInBehindNewerOnes)
    {
        using namespace ProblemListTestDetail;
        List list{};
        list.show("newest", "value", 30u);
        list.show("oldest", "value", 10u);
        list.show("middle", "value", 20u);

        EXPECT_EQ(idsOf(list), (Names{"newest", "middle", "oldest"}));
    }

    TEST(ProblemListTests, RanksCountFromTheNewest)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third", "fourth"}, sequence);

        EXPECT_EQ(list.rankOf("fourth"), std::optional<std::size_t>{0});
        EXPECT_EQ(list.rankOf("third"), std::optional<std::size_t>{1});
        EXPECT_EQ(list.rankOf("second"), std::optional<std::size_t>{2});
        EXPECT_EQ(list.rankOf("first"), std::optional<std::size_t>{3});
        EXPECT_TRUE(list.contains("second"));
        ASSERT_NE(list.find("second"), nullptr);
        EXPECT_EQ(*list.find("second"), "second-value");
    }

    TEST(ProblemListTests, RemovingOneKeepsTheOrderOfTheOthers)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third"}, sequence);

        EXPECT_TRUE(list.remove(std::string{"second"}));
        EXPECT_FALSE(list.remove(std::string{"second"}));
        EXPECT_FALSE(list.remove(std::string{"unknown"}));

        EXPECT_EQ(idsOf(list), (Names{"third", "first"}));
        EXPECT_FALSE(list.contains("second"));
        EXPECT_EQ(list.find("second"), nullptr);
        EXPECT_EQ(list.rankOf("first"), std::optional<std::size_t>{1});
    }

    TEST(ProblemListTests, ARemovedIdComesBackAsNew)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second"}, sequence);
        list.remove(std::string{"first"});

        EXPECT_TRUE(list.show("first", "back", ++sequence));
        EXPECT_EQ(idsOf(list), (Names{"first", "second"}));
    }

    TEST(ProblemListTests, RemovingManySkipsUnknownAndRepeatedIds)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third", "fourth", "fifth"}, sequence);

        EXPECT_EQ(list.remove(Names{"fourth", "unknown", "first", "fourth", "second"}), std::size_t{3});

        EXPECT_EQ(idsOf(list), (Names{"fifth", "third"}));
        EXPECT_EQ(list.size(), std::size_t{2});
        EXPECT_EQ(list.rankOf("third"), std::optional<std::size_t>{1});
        EXPECT_FALSE(list.contains("first"));
        EXPECT_FALSE(list.contains("second"));
        EXPECT_FALSE(list.contains("fourth"));
    }

    TEST(ProblemListTests, RemovingManyWithOneKnownIdRemovesIt)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third"}, sequence);

        EXPECT_EQ(list.remove(Names{"unknown", "second"}), std::size_t{1});
        EXPECT_EQ(list.remove(Names{"unknown"}), std::size_t{0});
        EXPECT_EQ(list.remove(Names{}), std::size_t{0});

        EXPECT_EQ(idsOf(list), (Names{"third", "first"}));
    }

    TEST(ProblemListTests, RemovingEveryIdEmptiesTheList)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third"}, sequence);

        EXPECT_EQ(list.remove(Names{"third", "first", "second"}), std::size_t{3});

        EXPECT_TRUE(list.empty());
        EXPECT_EQ(list.newest(), nullptr);
    }

    TEST(ProblemListTests, SlicesArePagesFromAnOffset)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        for (auto index = 0; index < 250; ++index)
            list.show(std::to_string(index), std::to_string(index), ++sequence);

        const auto first = list.slice(0u, 100u);
        ASSERT_EQ(first.size(), std::size_t{100});
        EXPECT_EQ(first.front(), "249");
        EXPECT_EQ(first.back(), "150");

        const auto second = list.slice(100u, 100u);
        ASSERT_EQ(second.size(), std::size_t{100});
        EXPECT_EQ(second.front(), "149");
        EXPECT_EQ(second.back(), "50");

        const auto last = list.slice(200u, 100u);
        ASSERT_EQ(last.size(), std::size_t{50});
        EXPECT_EQ(last.front(), "49");
        EXPECT_EQ(last.back(), "0");

        EXPECT_EQ(list.slice(250u, 100u), Names{});
        EXPECT_EQ(list.slice(1000u, 100u), Names{});
        EXPECT_EQ(list.slice(10u, 0u), Names{});
    }

    TEST(ProblemListTests, ANewProblemShiftsEveryPageByOne)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second", "third", "fourth"}, sequence);

        EXPECT_EQ(list.slice(2u, 2u), (Names{"second-value", "first-value"}));

        list.show("fifth", "fifth-value", ++sequence);

        EXPECT_EQ(list.slice(2u, 2u), (Names{"third-value", "second-value"}));
        EXPECT_EQ(list.rankOf("first"), std::optional<std::size_t>{4});
    }

    TEST(ProblemListTests, ClearForgetsEverything)
    {
        using namespace ProblemListTestDetail;
        List list{};
        std::uint64_t sequence = 0u;
        showEach(list, {"first", "second"}, sequence);
        list.clear();

        EXPECT_TRUE(list.empty());
        EXPECT_FALSE(list.contains("first"));
        EXPECT_TRUE(list.show("first", "again", ++sequence));
        EXPECT_EQ(list.size(), std::size_t{1});
    }
}

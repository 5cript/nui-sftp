#pragma once

#include <frontend/session_components/operation_queue/in_flight_row_list.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OperationQueueTests
{
    namespace InFlightRowListTestDetail
    {
        struct TestRow
        {
            TestRow() = default;
            TestRow(TestRow const&) = delete;
            TestRow(TestRow&&) = delete;
            TestRow& operator=(TestRow const&) = delete;
            TestRow& operator=(TestRow&&) = delete;
            ~TestRow()
            {
                *alive = false;
            }

            Nui::Observed<std::string> name{""};
            /** @brief Outlives the row, so an event can tell that its row is gone without reading it. */
            std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
        };

        void show(TestRow& row, std::optional<std::uint64_t> const& entryIndex)
        {
            const auto text = entryIndex ? std::to_string(*entryIndex) : std::string{};
            if (row.name.value() != text)
                row.name = text;
        }

        std::vector<std::string> namesOf(InFlightRowList<TestRow>& list)
        {
            std::vector<std::string> names{};
            for (auto const& row : list.rows().value())
                names.push_back(row->name.value());
            return names;
        }

        /**
         * @brief Attaches an event to the name of every row like a rendered text does. Nui's handler of a row that is
         *        gone detaches from the freed observed, which is what crashed the renderer; this one counts instead.
         */
        void renderRows(InFlightRowList<TestRow>& list, int& ranForDestroyedRow)
        {
            for (auto const& row : list.rows().value())
            {
                const auto eventId = Nui::globalEventContext.registerEvent(Nui::Event{
                    [alive = row->alive, &ranForDestroyedRow](std::size_t)
                    {
                        if (*alive)
                            return true;
                        ++ranForDestroyedRow;
                        return false;
                    },
                });
                row->name.attachEvent(eventId);
            }
        }

        class InFlightRowListTests : public ::testing::Test
        {
          protected:
            void TearDown() override
            {
                Nui::globalEventContext.reset();
            }
        };
    }

    using InFlightRowListTestDetail::InFlightRowListTests;

    TEST_F(InFlightRowListTests, RowsFollowTheEntriesInFlight)
    {
        using namespace InFlightRowListTestDetail;
        InFlightRowList<TestRow> list;
        list.update({1u, 2u, 3u}, show);

        EXPECT_EQ(namesOf(list), (std::vector<std::string>{"1", "2", "3"}));
    }

    TEST_F(InFlightRowListTests, AFreedRowInTheMiddleIsBlanked)
    {
        using namespace InFlightRowListTestDetail;
        InFlightRowList<TestRow> list;
        list.update({1u, 2u, 3u}, show);
        list.update({1u, 3u}, show);

        EXPECT_EQ(namesOf(list), (std::vector<std::string>{"1", "", "3"}));
    }

    TEST_F(InFlightRowListTests, ANewEntryTakesTheFreedRowBeforeAppending)
    {
        using namespace InFlightRowListTestDetail;
        InFlightRowList<TestRow> list;
        list.update({1u, 2u, 3u}, show);
        list.update({1u, 3u}, show);
        list.update({1u, 3u, 4u, 5u}, show);

        EXPECT_EQ(namesOf(list), (std::vector<std::string>{"1", "4", "3", "5"}));
    }

    TEST_F(InFlightRowListTests, ClearDropsEveryRow)
    {
        using namespace InFlightRowListTestDetail;
        InFlightRowList<TestRow> list;
        list.update({1u, 2u}, show);
        list.clear();

        EXPECT_TRUE(list.rows().value().empty());
    }

    TEST_F(InFlightRowListTests, ADroppedRowIsNotChangedBeforeItLeaves)
    {
        using namespace InFlightRowListTestDetail;
        int ranForDestroyedRow = 0;
        InFlightRowList<TestRow> list;
        list.update({1u, 2u, 3u}, show);
        Nui::globalEventContext.executeActiveEventsImmediately();
        renderRows(list, ranForDestroyedRow);

        list.update({1u}, show);
        Nui::globalEventContext.executeActiveEventsImmediately();

        EXPECT_EQ(namesOf(list), (std::vector<std::string>{"1"}));
        EXPECT_EQ(ranForDestroyedRow, 0);
    }
}

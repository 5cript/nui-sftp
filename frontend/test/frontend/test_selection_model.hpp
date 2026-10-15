#pragma once

#include <nui-file-explorer/side/selection_model.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <set>

namespace NuiFileExplorer::Test
{
    namespace SelectionModelTestDetail
    {
        inline auto everything()
        {
            return [](std::size_t)
            {
                return true;
            };
        }

        inline auto only(std::set<std::size_t> visible)
        {
            return [visible = std::move(visible)](std::size_t index)
            {
                return visible.contains(index);
            };
        }
    }

    TEST(SelectionModelTests, NothingIsSelectedAtFirst)
    {
        using namespace SelectionModelTestDetail;
        const SelectionModel selection{};
        EXPECT_TRUE(selection.selectedIndices(5, everything()).empty());
        EXPECT_FALSE(selection.activeIndex());
    }

    TEST(SelectionModelTests, SelectAllSelectsEverySelectableIndex)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.selectAll(4);
        EXPECT_EQ(selection.selectedIndices(4, everything()), (std::set<std::size_t>{0, 1, 2, 3}));
        EXPECT_EQ(selection.selectedIndices(4, only({1, 3})), (std::set<std::size_t>{1, 3}));
    }

    TEST(SelectionModelTests, SelectAllOfNothingSelectsNothing)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.collapseTo(2);
        selection.selectAll(0);
        EXPECT_TRUE(selection.selectedIndices(3, everything()).empty());
        EXPECT_FALSE(selection.anchor());
    }

    TEST(SelectionModelTests, ItemsHiddenDuringSelectAllStayUnselectedWhenTheyShowAgain)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        const auto filtered = only({2, 5});
        selection.selectAll(8);
        const auto shown = selection.selectedIndices(8, filtered);
        ASSERT_EQ(shown, (std::set<std::size_t>{2, 5}));

        selection.pin(shown);

        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{2, 5}));
    }

    TEST(SelectionModelTests, ItemsHiddenInsideAShiftRangeStayUnselectedWhenTheyShowAgain)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        const auto filtered = only({1, 4, 6});
        selection.collapseTo(1);
        selection.extendTo(6);
        selection.pin(selection.selectedIndices(10, filtered));

        EXPECT_EQ(selection.selectedIndices(10, everything()), (std::set<std::size_t>{1, 4, 6}));
    }

    TEST(SelectionModelTests, ItemsHiddenByANarrowerFilterAreDropped)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.selectAll(6);
        selection.pin(selection.selectedIndices(6, only({0, 3, 4})));
        selection.pin(selection.selectedIndices(6, only({3})));

        EXPECT_EQ(selection.selectedIndices(6, everything()), (std::set<std::size_t>{3}));
    }

    TEST(SelectionModelTests, PinKeepsTheAnchorWhenItIsSelected)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.collapseTo(2);
        selection.extendTo(5);
        selection.pin(selection.selectedIndices(8, only({2, 4})));

        EXPECT_EQ(selection.anchor(), 2u);
        EXPECT_FALSE(selection.flag());

        selection.extendTo(7);
        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{2, 3, 4, 5, 6, 7}));
    }

    TEST(SelectionModelTests, PinDropsAnAnchorThatIsNotSelected)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.selectAll(5);
        selection.pin(selection.selectedIndices(5, only({3})));

        EXPECT_FALSE(selection.anchor());
        selection.extendTo(1);
        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{1, 3}));
    }

    TEST(SelectionModelTests, CollapseSelectsOnlyOneIndex)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.selectAll(5);
        selection.add(7);
        selection.collapseTo(3);

        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{3}));
        EXPECT_EQ(selection.activeIndex(), 3u);
    }

    TEST(SelectionModelTests, ExtendSelectsTheRangeInEitherDirection)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.collapseTo(4);
        selection.extendTo(1);

        EXPECT_EQ(selection.selectedIndices(6, everything()), (std::set<std::size_t>{1, 2, 3, 4}));
        EXPECT_EQ(selection.anchor(), 4u);
        EXPECT_EQ(selection.activeIndex(), 1u);
    }

    TEST(SelectionModelTests, ExtendWithoutAnchorStartsAtTheIndex)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.extendTo(2);

        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{2}));
    }

    TEST(SelectionModelTests, ExtendKeepsTheMask)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.collapseTo(0);
        selection.add(5);
        selection.extendTo(2);

        EXPECT_EQ(selection.selectedIndices(6, everything()), (std::set<std::size_t>{0, 1, 2, 5}));
    }

    TEST(SelectionModelTests, ToggleMaskedCoversItsFourCases)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.replaceRange(0, 2);

        selection.toggleMasked(1);
        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{0, 2}));
        selection.toggleMasked(1);
        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{0, 1, 2}));
        selection.toggleMasked(4);
        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{0, 1, 2, 4}));
        selection.toggleMasked(4);
        EXPECT_EQ(selection.selectedIndices(5, everything()), (std::set<std::size_t>{0, 1, 2}));
        EXPECT_EQ(selection.anchor(), 0u);
        EXPECT_EQ(selection.flag(), 2u);
    }

    TEST(SelectionModelTests, RemovePunchesAnIndexOutOfTheRange)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.replaceRange(1, 3);
        selection.add(6);
        selection.remove(2);
        selection.remove(6);

        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{1, 3}));
        selection.add(2);
        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{1, 2, 3}));
    }

    TEST(SelectionModelTests, ReplaceRangeDropsTheMask)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.add(7);
        selection.replaceRange(3, 1);

        EXPECT_EQ(selection.selectedIndices(8, everything()), (std::set<std::size_t>{1, 2, 3}));
    }

    TEST(SelectionModelTests, ClearForgetsEverything)
    {
        using namespace SelectionModelTestDetail;
        SelectionModel selection{};
        selection.replaceRange(0, 3);
        selection.add(5);
        selection.clear();

        EXPECT_TRUE(selection.selectedIndices(6, everything()).empty());
        EXPECT_FALSE(selection.activeIndex());
    }
}

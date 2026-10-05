#pragma once

#include <utility/keyed_diff.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <random>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class KeyedDiffTests : public Test
    {
      protected:
        struct Element
        {
            int key{0};
            int value{0};

            bool operator==(Element const&) const = default;
        };

        static std::vector<KeyedEdit> editsFor(std::vector<Element> const& current, std::vector<Element> const& target)
        {
            return keyedEdits(current, target, [](Element const& element) {
                return element.key;
            });
        }

        /**
         * @brief Applies the edits the way an observed list would, checking every index on the way.
         */
        static std::vector<Element>
        applied(std::vector<Element> current, std::vector<Element> const& target, std::vector<KeyedEdit> const& edits)
        {
            for (auto const& edit : edits)
            {
                switch (edit.kind)
                {
                    case KeyedEdit::Kind::Erase:
                        EXPECT_LT(edit.index, current.size());
                        current.erase(current.begin() + static_cast<std::ptrdiff_t>(edit.index));
                        break;
                    case KeyedEdit::Kind::Insert:
                        EXPECT_LE(edit.index, current.size());
                        current.insert(current.begin() + static_cast<std::ptrdiff_t>(edit.index), target[edit.source]);
                        break;
                    case KeyedEdit::Kind::Update:
                        EXPECT_LT(edit.index, current.size());
                        current[edit.index] = target[edit.source];
                        break;
                }
            }
            return current;
        }

        static std::vector<Element> sequence(std::vector<int> const& keys)
        {
            std::vector<Element> elements{};
            for (const auto key : keys)
                elements.push_back(Element{.key = key, .value = key * 10});
            return elements;
        }

        static void expectReachesTarget(std::vector<Element> const& current, std::vector<Element> const& target)
        {
            EXPECT_EQ(applied(current, target, editsFor(current, target)), target);
        }
    };

    TEST_F(KeyedDiffTests, IdenticalSequencesNeedNoEdits)
    {
        const auto elements = sequence({1, 2, 3, 4});
        EXPECT_TRUE(editsFor(elements, elements).empty());
    }

    TEST_F(KeyedDiffTests, ChangedElementIsUpdatedInPlace)
    {
        const auto current = sequence({1, 2, 3});
        auto target = current;
        target[1].value = 99;

        EXPECT_EQ(
            editsFor(current, target),
            (std::vector<KeyedEdit>{{.kind = KeyedEdit::Kind::Update, .index = 1, .source = 1}})
        );
    }

    TEST_F(KeyedDiffTests, NewElementsAreInserted)
    {
        const auto current = sequence({2, 3});
        const auto target = sequence({1, 2, 3, 4});

        EXPECT_EQ(
            editsFor(current, target),
            (std::vector<KeyedEdit>{
                {.kind = KeyedEdit::Kind::Insert, .index = 0, .source = 0},
                {.kind = KeyedEdit::Kind::Insert, .index = 3, .source = 3},
            })
        );
    }

    TEST_F(KeyedDiffTests, RemovedElementsAreErasedBackToFront)
    {
        const auto current = sequence({1, 2, 3, 4, 5});
        const auto target = sequence({2, 4});

        EXPECT_EQ(
            editsFor(current, target),
            (std::vector<KeyedEdit>{
                {.kind = KeyedEdit::Kind::Erase, .index = 4},
                {.kind = KeyedEdit::Kind::Erase, .index = 2},
                {.kind = KeyedEdit::Kind::Erase, .index = 0},
            })
        );
    }

    TEST_F(KeyedDiffTests, ElementMovedToTheFrontCostsOneEraseAndOneInsert)
    {
        const auto current = sequence({1, 2, 3, 4, 5, 6});
        const auto target = sequence({5, 1, 2, 3, 4, 6});

        const auto edits = editsFor(current, target);
        EXPECT_EQ(edits.size(), 2u);
        EXPECT_EQ(applied(current, target, edits), target);
    }

    TEST_F(KeyedDiffTests, ElementMovedToTheBackCostsOneEraseAndOneInsert)
    {
        const auto current = sequence({1, 2, 3, 4, 5, 6});
        const auto target = sequence({2, 3, 4, 5, 1, 6});

        const auto edits = editsFor(current, target);
        EXPECT_EQ(edits.size(), 2u);
        EXPECT_EQ(applied(current, target, edits), target);
    }

    TEST_F(KeyedDiffTests, MovedAndChangedElementIsInsertedWithItsNewValue)
    {
        const auto current = sequence({1, 2, 3});
        auto target = sequence({3, 1, 2});
        target[0].value = 7;

        expectReachesTarget(current, target);
    }

    TEST_F(KeyedDiffTests, EmptySequences)
    {
        EXPECT_TRUE(editsFor({}, {}).empty());
        expectReachesTarget({}, sequence({1, 2}));
        expectReachesTarget(sequence({1, 2}), {});
    }

    TEST_F(KeyedDiffTests, ReversedSequenceReachesTheTarget)
    {
        expectReachesTarget(sequence({1, 2, 3, 4, 5, 6, 7}), sequence({7, 6, 5, 4, 3, 2, 1}));
    }

    TEST_F(KeyedDiffTests, DuplicateKeysInCurrentStillReachTheTarget)
    {
        expectReachesTarget(sequence({1, 2, 2, 3}), sequence({1, 2, 3}));
    }

    TEST_F(KeyedDiffTests, RandomEditsAlwaysReachTheTarget)
    {
        std::mt19937 random{4711u};
        for (int round = 0; round != 500; ++round)
        {
            std::vector<int> pool(30);
            std::iota(pool.begin(), pool.end(), 0);

            std::ranges::shuffle(pool, random);
            const auto currentSize = std::uniform_int_distribution<std::size_t>{0, pool.size()}(random);
            const auto current = sequence({pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(currentSize)});

            std::ranges::shuffle(pool, random);
            const auto targetSize = std::uniform_int_distribution<std::size_t>{0, pool.size()}(random);
            auto target = sequence({pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(targetSize)});
            for (auto& element : target)
            {
                if (std::uniform_int_distribution<int>{0, 3}(random) == 0)
                    element.value = -element.value - 1;
            }

            const auto edits = editsFor(current, target);
            ASSERT_EQ(applied(current, target, edits), target) << "round " << round;
            EXPECT_LE(edits.size(), current.size() + target.size()) << "round " << round;
        }
    }
}

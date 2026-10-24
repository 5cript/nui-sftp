#include "test_attention_state.hpp"
#include "test_drop_text.hpp"
#include "test_in_flight_rows.hpp"
#include "test_item_names.hpp"
#include "test_item_order.hpp"
#include "test_numbered_saves.hpp"
#include "test_overwrite_decisions.hpp"
#include "test_selection_model.hpp"
#include "test_side_paths.hpp"
#include "test_tracking_model.hpp"
#include "test_transfer_target.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

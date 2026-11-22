#include "pi/app/selection.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(SelectionTest, ClampsOutOfRangeIndexes)
{
    EXPECT_EQ(clamp_selection(0, 0), 0U);
    EXPECT_EQ(clamp_selection(5, 0), 0U);
    EXPECT_EQ(clamp_selection(0, 3), 0U);
    EXPECT_EQ(clamp_selection(2, 3), 2U);
    EXPECT_EQ(clamp_selection(9, 3), 2U);
}

TEST(SelectionTest, ShowsEverythingWhenListFits)
{
    const auto window = selection_window(5, 3, 8);
    EXPECT_EQ(window.offset, 0U);
    EXPECT_EQ(window.visible, 5U);
}

TEST(SelectionTest, ScrollsToKeepSelectionVisible)
{
    // 选中第 10 项（下标 9）：窗口整体下移，选中项落在最后一行
    const auto window = selection_window(10, 9, 8);
    EXPECT_EQ(window.visible, 8U);
    EXPECT_EQ(window.offset, 2U);

    // 选中项仍在首屏内时不滚动
    const auto first_page = selection_window(10, 7, 8);
    EXPECT_EQ(first_page.offset, 0U);
    EXPECT_EQ(first_page.visible, 8U);

    // 远离首屏的选择
    const auto middle = selection_window(100, 50, 8);
    EXPECT_EQ(middle.visible, 8U);
    EXPECT_LT(middle.offset, 50U);
    EXPECT_GE(middle.offset + middle.visible, 51U);
}

TEST(SelectionTest, HandlesEmptyListsAndZeroVisibleLines)
{
    auto window = selection_window(0, 0, 8);
    EXPECT_EQ(window.offset, 0U);
    EXPECT_EQ(window.visible, 0U);

    window = selection_window(10, 0, 0);
    EXPECT_EQ(window.offset, 0U);
    EXPECT_EQ(window.visible, 0U);
}

}  // namespace
}  // namespace pi

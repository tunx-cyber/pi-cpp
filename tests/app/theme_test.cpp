#include "pi/app/theme.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(ThemeTest, DefaultThemeWrapsTextWithColorAndReset)
{
    const auto& theme = theme_by_name("default");
    ASSERT_TRUE(theme.colored());
    EXPECT_EQ(styled(theme, theme.error, "boom"), "\033[31mboom\033[0m");
    // 未指定颜色时原样返回
    EXPECT_EQ(styled(theme, "", "plain"), "plain");
}

TEST(ThemeTest, MonoThemeDisablesColors)
{
    const auto& theme = theme_by_name("mono");
    EXPECT_FALSE(theme.colored());
    EXPECT_EQ(styled(theme, "\033[31m", "boom"), "boom");
}

TEST(ThemeTest, UnknownThemeFallsBackToDefault)
{
    EXPECT_EQ(theme_by_name("no-such-theme").name, "default");
}

}  // namespace
}  // namespace pi

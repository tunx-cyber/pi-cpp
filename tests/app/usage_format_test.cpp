#include "pi/app/usage_format.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(UsageFormatTest, FormatsTokenCountWithThousandsSeparators)
{
    EXPECT_EQ(format_token_count(0), "0");
    EXPECT_EQ(format_token_count(7), "7");
    EXPECT_EQ(format_token_count(999), "999");
    EXPECT_EQ(format_token_count(1000), "1,000");
    EXPECT_EQ(format_token_count(1234567), "1,234,567");
    EXPECT_EQ(format_token_count(-1234), "-1,234");
    // INT64_MIN 取绝对值会溢出，必须按无符号处理
    EXPECT_EQ(format_token_count(INT64_MIN), "-9,223,372,036,854,775,808");
}

TEST(UsageFormatTest, FormatsAllFourTokenCounts)
{
    Usage usage;
    usage.input = 12345;
    usage.output = 6789;
    usage.cacheRead = 100000;
    usage.cacheWrite = 2000;
    usage.totalTokens = 121134;

    const std::string text = format_token_usage(usage);
    EXPECT_EQ(text, "input=12,345 output=6,789 cacheRead=100,000 cacheWrite=2,000 total=121,134");
    EXPECT_EQ(text.find("¥"), std::string::npos);
}

TEST(UsageFormatTest, FormatsEmptyUsage)
{
    EXPECT_EQ(format_token_usage(Usage{}), "input=0 output=0 cacheRead=0 cacheWrite=0 total=0");
}

}  // namespace
}  // namespace pi

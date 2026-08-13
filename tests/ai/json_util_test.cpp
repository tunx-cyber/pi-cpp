#include "pi/ai/json_util.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(JsonUtilTest, ParsesValidJson)
{
    const Json parsed = parse_streaming_json("{\"a\":1}");
    EXPECT_EQ(parsed["a"], 1);
}

TEST(JsonUtilTest, EmptyAndGarbageFallBackToEmptyObject)
{
    EXPECT_TRUE(parse_streaming_json("").empty());
    EXPECT_TRUE(parse_streaming_json("not json at all").empty());
}

TEST(JsonUtilTest, TruncatedValueFallsBackToEmptyObject)
{
    // 未闭合的字符串/对象没有任何合法前缀 → 空对象兜底
    EXPECT_TRUE(parse_streaming_json("{\"content\":\"partial").empty());
    EXPECT_TRUE(parse_streaming_json("{\"a\":123").empty());
}

TEST(JsonUtilTest, TrailingJunkReturnsLongestValidPrefix)
{
    // 完整对象/数组后跟残余 → 返回对象/数组本身
    EXPECT_EQ(parse_streaming_json("{\"a\":1}junk")["a"], 1);
    EXPECT_EQ(parse_streaming_json("[1,2]rest").size(), 2u);
    EXPECT_EQ(parse_streaming_json("true something").type(), Json::value_t::boolean);
}

TEST(JsonUtilTest, RepairFixesControlCharactersInStrings)
{
    const Json parsed = parse_streaming_json("{\"a\":\"line1\nline2\"}");
    EXPECT_EQ(parsed["a"], "line1\nline2");
}

TEST(JsonUtilTest, LargeUnclosedStringIsHandled)
{
    // 大段未闭合字符串（write 工具的流式参数形态）：快速兜底，不触发逐字符重解析
    const std::string big = "{\"content\":\"" + std::string(10000, 'x');
    EXPECT_TRUE(parse_streaming_json(big).empty());
}

}  // namespace
}  // namespace pi

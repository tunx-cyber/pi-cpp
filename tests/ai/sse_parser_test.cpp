#include "pi/ai/sse_parser.h"

#include <gtest/gtest.h>

#include <string>

namespace pi
{
namespace
{

TEST(SseParserTest, ParsesSingleEvent)
{
    const auto events = parse_sse("data: {\"a\":1}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "{\"a\":1}");
    EXPECT_FALSE(events[0].event.has_value());
}

TEST(SseParserTest, ParsesMultipleEvents)
{
    const auto events = parse_sse("data: one\n\n\ndata: two\n\n");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].data, "one");
    EXPECT_EQ(events[1].data, "two");
}

TEST(SseParserTest, HandlesEventFieldAndId)
{
    const auto events = parse_sse("event: error\ndata: boom\nid: 42\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].event.value_or(""), "error");
    EXPECT_EQ(events[0].id, "42");
    EXPECT_EQ(events[0].data, "boom");
}

TEST(SseParserTest, JoinsMultiLineData)
{
    const auto events = parse_sse("data: first\ndata: second\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "first\nsecond");
}

TEST(SseParserTest, IgnoresComments)
{
    const auto events = parse_sse(": comment\ndata: value\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "value");
}

TEST(SseParserTest, HandlesCrlf)
{
    const auto events = parse_sse("data: value\r\n\r\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "value");
}

TEST(SseParserTest, DataWithoutSpaceAfterColon)
{
    const auto events = parse_sse("data:{\"x\":1}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "{\"x\":1}");
}

TEST(SseParserTest, FieldWithoutColonIsIgnoredAsData)
{
    // 无冒号的字段名行按 data 处理（标准中未知字段被忽略）
    const auto events = parse_sse("nonsense\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "");
}

TEST(SseParserTest, IncrementalFeedingAcrossChunks)
{
    SseParser parser;
    auto events = parser.feed("data: par");
    EXPECT_TRUE(events.empty());
    events = parser.feed("tial\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "partial");
}

TEST(SseParserTest, FinalizeFlushesTrailingEvent)
{
    SseParser parser;
    parser.feed("data: trailing\n");
    auto events = parser.finalize();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "trailing");
}

TEST(SseParserTest, ParsesDoneSentinel)
{
    const auto events = parse_sse("data: [DONE]\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "[DONE]");
}

}  // namespace
}  // namespace pi

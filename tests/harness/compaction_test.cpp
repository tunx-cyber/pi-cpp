#include "pi/harness/compaction.h"

#include <gtest/gtest.h>

#include "test_utils/scripted_transport.h"

namespace pi
{
namespace
{

// 对照 compaction.test.ts 的核心行为

SessionTreeEntry message_entry(const std::string& id, const AgentMessage& message)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::Message;
    entry.id = id;
    entry.message = message;
    return entry;
}

std::string long_text(int length) { return std::string(length, 'x'); }

TEST(CompactionTest, EstimateTokensUserMessage)
{
    AgentMessage message = Message::user(long_text(100));
    // chars/4 → 25
    EXPECT_EQ(estimate_tokens(message), 25);
}

TEST(CompactionTest, EstimateTokensImageBlock)
{
    AgentMessage message;
    message.role = Role::User;
    ContentBlock image;
    image.type = BlockType::Image;
    image.data = "AAAA";
    message.content.push_back(image);
    // 4800 图 + 0 文本 → 1200
    EXPECT_EQ(estimate_tokens(message), 1200);
}

TEST(CompactionTest, ShouldCompactThreshold)
{
    CompactionSettings settings = default_compaction_settings();
    settings.enabled = true;
    settings.reserveTokens = 10000;
    // contextWindow=100000, tokens=90001 > 90000 → compact
    EXPECT_TRUE(should_compact(90001, 100000, settings));
    EXPECT_FALSE(should_compact(90000, 100000, settings));
    settings.enabled = false;
    EXPECT_FALSE(should_compact(999999, 100000, settings));
}

TEST(CompactionTest, EstimateContextTokensUsesLastAssistantUsage)
{
    std::vector<AgentMessage> messages;
    messages.push_back(Message::user("a"));

    AgentMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::Stop;
    assistant.usage.input = 100;
    assistant.usage.output = 50;
    assistant.usage.cacheRead = 10;
    assistant.usage.cacheWrite = 0;
    assistant.usage.totalTokens = 160;
    messages.push_back(assistant);

    // 后续 40 字符 → 10 tokens；total = 160 + 10 = 170
    messages.push_back(Message::user(long_text(40)));

    const auto estimate = estimate_context_tokens(messages);
    EXPECT_EQ(estimate.tokens, 170);
    EXPECT_EQ(estimate.usageTokens, 160);
    EXPECT_EQ(estimate.trailingTokens, 10);
    EXPECT_EQ(estimate.lastUsageIndex, 1);
}

TEST(CompactionTest, FindCutPointKeepsRecentTokens)
{
    std::vector<SessionTreeEntry> entries;
    for (int i = 0; i < 10; ++i)
    {
        entries.push_back(message_entry("e" + std::to_string(i), Message::user(long_text(40))));
    }
    const auto cut = find_cut_point(entries, 0, static_cast<int>(entries.size()), 100);
    // 每条约 10 tokens；keep 100 → 大约保留 10 条 → 全部保留
    EXPECT_EQ(cut.firstKeptEntryIndex, 0);
    EXPECT_FALSE(cut.isSplitTurn);
}

TEST(CompactionTest, PrepareCompactionSkipsWhenLastIsCompaction)
{
    std::vector<SessionTreeEntry> entries;
    entries.push_back(message_entry("e0", Message::user("hi")));
    SessionTreeEntry compaction;
    compaction.type = SessionTreeEntry::Type::Compaction;
    compaction.id = "c0";
    compaction.summary = "summary";
    entries.push_back(compaction);

    const auto preparation = prepare_compaction(entries, default_compaction_settings());
    ASSERT_TRUE(preparation.ok);
    EXPECT_FALSE(preparation.value.has_value());
}

TEST(CompactionTest, PrepareCompactionSplitTurnDetection)
{
    // 大体积 user 消息 + 后续 assistant → 切点应在 user 之前（不分割回合）
    std::vector<SessionTreeEntry> entries;
    entries.push_back(message_entry("u0", Message::user(long_text(20000))));  // 5000 tokens
    AgentMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::Stop;
    assistant.content.push_back(ContentBlock{});
    assistant.content.back().type = BlockType::Text;
    assistant.content.back().text = "ok";
    entries.push_back(message_entry("a0", assistant));

    CompactionSettings settings = default_compaction_settings();
    settings.keepRecentTokens = 500;  // 只保留最近 500 tokens → 切点在 u0

    const auto preparation = prepare_compaction(entries, settings);
    ASSERT_TRUE(preparation.ok);
    ASSERT_TRUE(preparation.value.has_value());
    EXPECT_EQ(preparation.value->firstKeptEntryId, "u0");
    EXPECT_TRUE(preparation.value->messagesToSummarize.empty());
}

TEST(CompactionTest, ExtractFileOpsFromMessages)
{
    AgentMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::ToolUse;
    ContentBlock read_call;
    read_call.type = BlockType::ToolCall;
    read_call.name = "read";
    read_call.arguments = Json{{"path", "a.txt"}};
    ContentBlock edit_call;
    edit_call.type = BlockType::ToolCall;
    edit_call.name = "edit";
    edit_call.arguments = Json{{"path", "b.txt"}};
    assistant.content = {read_call, edit_call};

    FileOperations ops;
    extract_file_ops_from_message(assistant, ops);
    EXPECT_TRUE(ops.read.count("a.txt"));
    EXPECT_TRUE(ops.edited.count("b.txt"));

    const auto [read_files, modified_files] = compute_file_lists(ops);
    ASSERT_EQ(read_files.size(), 1u);
    EXPECT_EQ(read_files[0], "a.txt");
    ASSERT_EQ(modified_files.size(), 1u);
    EXPECT_EQ(modified_files[0], "b.txt");
}

TEST(CompactionTest, FormatFileOperations)
{
    const std::string formatted = format_file_operations({"a.txt"}, {"b.txt"});
    EXPECT_NE(formatted.find("<read-files>"), std::string::npos);
    EXPECT_NE(formatted.find("<modified-files>"), std::string::npos);
    EXPECT_NE(formatted.find("a.txt"), std::string::npos);
    EXPECT_NE(formatted.find("b.txt"), std::string::npos);

    EXPECT_TRUE(format_file_operations({}, {}).empty());
}

TEST(CompactionTest, SerializeConversation)
{
    std::vector<Message> messages;
    messages.push_back(Message::user("first question"));
    AgentMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::Stop;
    assistant.content.push_back(ContentBlock{});
    assistant.content.back().type = BlockType::Text;
    assistant.content.back().text = "the answer";
    messages.push_back(assistant);
    messages.push_back(Message::toolResult("c1", "bash", "ls output", false));

    const std::string serialized = serialize_conversation(messages);
    EXPECT_NE(serialized.find("[User]: first question"), std::string::npos);
    EXPECT_NE(serialized.find("[Assistant]: the answer"), std::string::npos);
    EXPECT_NE(serialized.find("[Tool result]: ls output"), std::string::npos);
}

// ---------- 请求体级回归（用 ScriptedTransport 捕获实际发出的 prompt） ----------

namespace
{

std::vector<AgentMessage> two_message_conversation()
{
    std::vector<AgentMessage> messages;
    messages.push_back(Message::user("first question"));
    AgentMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::Stop;
    assistant.content.push_back(ContentBlock{});
    assistant.content.back().type = BlockType::Text;
    assistant.content.back().text = "the answer";
    messages.push_back(assistant);
    return messages;
}

}  // namespace

TEST(CompactionTest, GenerateSummarySendsConversationExactlyOnce)
{
    // 回归：此前 generate_summary_internal 会再包一层 <conversation>，
    // 导致对话内容在摘要请求中出现两次（token 翻倍）。
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::text_turn("the summary"));

    const auto result =
        generate_summary(two_message_conversation(), pi_test::scripted_model(), 10000, transport,
                         "test-key", nullptr);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value, "the summary");

    const auto& received = transport->received_messages();
    ASSERT_EQ(received.size(), 1u);
    ASSERT_EQ(received[0].size(), 1u);
    const std::string prompt = received[0][0].text_content();
    EXPECT_EQ(prompt.find("[User]: first question"), prompt.rfind("[User]: first question"));
    EXPECT_EQ(prompt.find("[Assistant]: the answer"), prompt.rfind("[Assistant]: the answer"));
    EXPECT_NE(prompt.find("<conversation>"), std::string::npos);
    EXPECT_NE(prompt.find("</conversation>"), std::string::npos);
}

TEST(CompactionTest, UpdateSummaryIncludesPreviousSummaryOnce)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::text_turn("updated summary"));

    const auto result = generate_summary({Message::user("new message")}, pi_test::scripted_model(),
                                         10000, transport, "test-key", nullptr, "", "prior summary");
    ASSERT_TRUE(result.ok) << result.error.message;

    const auto& received = transport->received_messages();
    ASSERT_EQ(received.size(), 1u);
    const std::string prompt = received[0][0].text_content();
    // 历史摘要正文只注入一次（<previous-summary> 标签名还会出现在 update 模板
    // 的说明文字里，因此断言正文内容而不是标签）。
    EXPECT_EQ(prompt.find("prior summary"), prompt.rfind("prior summary"));
    // 增量摘要使用 update 模板而不是首次摘要模板
    EXPECT_NE(prompt.find("UPDATE"), std::string::npos);
    EXPECT_EQ(prompt.find("context checkpoint"), std::string::npos);
}

}  // namespace
}  // namespace pi

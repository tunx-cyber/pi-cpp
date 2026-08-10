#include "pi/harness/session.h"

#include <gtest/gtest.h>

#include <cstdio>

#include <filesystem>
#include <string>

#include "pi/harness/env.h"
#include "pi/harness/jsonl_repo.h"

namespace pi
{
namespace
{

class SessionTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/pi_session_test_XXXXXX";
        root_ = mkdtemp(tmpl);
        fs_ = std::make_unique<PosixFileSystem>(root_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    std::string root_;
    std::unique_ptr<PosixFileSystem> fs_;
};

TEST_F(SessionTest, RoundTripWriteOpenAndBuildContext)
{
    JsonlSessionRepo repo(*fs_, root_ + "/sessions");
    const auto created = repo.create(root_);
    ASSERT_TRUE(created.ok) << created.error.message;
    auto session = created.value;

    // 写入一轮对话
    const auto user_id = session.append_message(Message::user("hello"));
    ASSERT_TRUE(user_id.ok);

    Message assistant;
    assistant.role = Role::Assistant;
    assistant.api = "openai-completions";
    assistant.provider = "deepseek";
    assistant.model = "deepseek-v4-flash";
    assistant.stopReason = StopReason::Stop;
    assistant.usage.input = 10;
    assistant.usage.output = 5;
    assistant.usage.totalTokens = 15;
    assistant.content.push_back(ContentBlock{});
    assistant.content.back().type = BlockType::Text;
    assistant.content.back().text = "hi there";
    const auto assistant_id = session.append_message(assistant);
    ASSERT_TRUE(assistant_id.ok);

    session.append_model_change("deepseek", "deepseek-v4-flash");
    session.append_thinking_level_change("high");

    // 重新打开
    const auto metadata = session.metadata();
    const auto reopened = repo.open(metadata);
    ASSERT_TRUE(reopened.ok) << reopened.error.message;

    const auto context = reopened.value.build_context();
    ASSERT_TRUE(context.ok) << context.error.message;
    const auto& ctx = context.value;
    ASSERT_EQ(ctx.messages.size(), 2u);
    EXPECT_EQ(ctx.messages[0].role, Role::User);
    EXPECT_EQ(ctx.messages[0].text_content(), "hello");
    EXPECT_EQ(ctx.messages[1].role, Role::Assistant);
    EXPECT_EQ(ctx.messages[1].text_content(), "hi there");
    EXPECT_EQ(ctx.messages[1].usage.output, 5);
    ASSERT_TRUE(ctx.model.has_value());
    EXPECT_EQ(ctx.model->first, "deepseek");
    EXPECT_EQ(ctx.model->second, "deepseek-v4-flash");
    EXPECT_EQ(ctx.thinkingLevel, "high");
}

TEST_F(SessionTest, ListFindsSessionsSortedByRecency)
{
    JsonlSessionRepo repo(*fs_, root_ + "/sessions");
    const auto first = repo.create(root_);
    ASSERT_TRUE(first.ok);
    const auto second = repo.create(root_);
    ASSERT_TRUE(second.ok);

    const auto sessions = repo.list(root_);
    ASSERT_TRUE(sessions.ok) << sessions.error.message;
    ASSERT_EQ(sessions.value.size(), 2u);
    // 新创建的在前
    EXPECT_EQ(sessions.value[0].id, second.value.metadata().id);
    EXPECT_EQ(sessions.value[1].id, first.value.metadata().id);
}

TEST_F(SessionTest, BranchAndLabelSupport)
{
    JsonlSessionRepo repo(*fs_, root_ + "/sessions");
    const auto created = repo.create(root_);
    ASSERT_TRUE(created.ok);
    auto session = created.value;

    const auto user_id = session.append_message(Message::user("first"));
    const auto label = session.append_label(user_id.value, "my label");
    ASSERT_TRUE(label.ok);

    const auto label_value = session.get_label(user_id.value);
    ASSERT_TRUE(label_value.ok);
    EXPECT_EQ(label_value.value.value_or(""), "my label");

    // move_to 记录分支
    const auto moved = session.move_to("", "branch summary");
    ASSERT_TRUE(moved.ok);
    const auto branch = session.get_branch();
    ASSERT_TRUE(branch.ok);
    // 最后是 branch_summary 条目
    EXPECT_EQ(branch.value.back().type, SessionTreeEntry::Type::BranchSummary);
}

TEST_F(SessionTest, InvalidSessionFileRejected)
{
    fs_->write_file(root_ + "/bad.jsonl", "not json\n");
    const auto storage = JsonlSessionStorage::open(*fs_, root_ + "/bad.jsonl");
    EXPECT_FALSE(storage.ok);
}

TEST_F(SessionTest, MessageSerializationRoundTripAllBlockTypes)
{
    Message assistant;
    assistant.role = Role::Assistant;
    assistant.model = "m";
    assistant.provider = "p";
    assistant.api = "a";
    assistant.stopReason = StopReason::ToolUse;
    assistant.usage.input = 1;
    assistant.usage.output = 2;
    assistant.usage.cacheRead = 3;
    assistant.usage.cacheWrite = 4;
    assistant.usage.totalTokens = 10;

    ContentBlock thinking;
    thinking.type = BlockType::Thinking;
    thinking.thinking = "deep thought";
    thinking.thinkingSignature = "reasoning_content";
    ContentBlock call;
    call.type = BlockType::ToolCall;
    call.id = "call_1";
    call.name = "bash";
    call.arguments = Json{{"command", "ls -la"}};
    ContentBlock text;
    text.type = BlockType::Text;
    text.text = "text here";
    assistant.content = {thinking, call, text};

    const Json wire = message_to_session_json(assistant);
    const Message restored = message_from_session_json(wire);

    EXPECT_EQ(restored.stopReason, StopReason::ToolUse);
    ASSERT_EQ(restored.content.size(), 3u);
    EXPECT_EQ(restored.content[0].type, BlockType::Thinking);
    EXPECT_EQ(restored.content[0].thinking, "deep thought");
    EXPECT_EQ(restored.content[1].type, BlockType::ToolCall);
    EXPECT_EQ(restored.content[1].arguments["command"], "ls -la");
    EXPECT_EQ(restored.content[2].type, BlockType::Text);
    EXPECT_EQ(restored.usage.input, 1);
    EXPECT_EQ(restored.usage.totalTokens, 10);
}

TEST_F(SessionTest, RepoCreateUsesCwdEncoding)
{
    JsonlSessionRepo repo(*fs_, root_ + "/sessions");
    const auto created = repo.create("/some/path/with spaces");
    ASSERT_TRUE(created.ok);
    const auto sessions = repo.list("/some/path/with spaces");
    ASSERT_TRUE(sessions.ok);
    EXPECT_EQ(sessions.value.size(), 1u);
    // 其他 cwd 下看不到
    const auto others = repo.list("/other/path");
    ASSERT_TRUE(others.ok);
    EXPECT_TRUE(others.value.empty());
}

}  // namespace
}  // namespace pi

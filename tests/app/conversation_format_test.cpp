#include "pi/app/conversation_format.h"

#include <gtest/gtest.h>

#include "pi/ai/types.h"

namespace pi
{
namespace
{

AgentMessage user_message(const std::string& text) { return Message::user(text); }

AgentMessage assistant_message(const std::string& text)
{
    AgentMessage message;
    message.role = Role::Assistant;
    ContentBlock block;
    block.type = BlockType::Text;
    block.text = text;
    message.content.push_back(block);
    return message;
}

TEST(ConversationFormatTest, FormatsUserAssistantAndToolCalls)
{
    AgentMessage assistant = assistant_message("hello");
    ContentBlock call;
    call.type = BlockType::ToolCall;
    call.id = "c1";
    call.name = "bash";
    call.arguments = Json{{"command", "ls"}};
    assistant.content.push_back(call);

    Message tool;
    tool.role = Role::ToolResult;
    ContentBlock result;
    result.type = BlockType::Text;
    result.text = "file.txt";
    tool.content.push_back(result);

    const auto theme = make_mono_theme();
    const std::string text = format_conversation({user_message("hi"), assistant, tool}, theme);
    EXPECT_NE(text.find("[you]\nhi"), std::string::npos);
    EXPECT_NE(text.find("[assistant]\nhello"), std::string::npos);
    EXPECT_NE(text.find("[tool] bash"), std::string::npos);
    EXPECT_NE(text.find("[tool result] file.txt"), std::string::npos);
    // mono 主题不带 ANSI 转义
    EXPECT_EQ(text.find("\033["), std::string::npos);
}

TEST(ConversationFormatTest, EmptySessionIsExplicit)
{
    const auto text = format_conversation({}, make_mono_theme());
    EXPECT_NE(text.find("空会话"), std::string::npos);
}

TEST(ConversationFormatTest, TruncatesHugeMessages)
{
    const std::string huge(kConversationMaxChars + 100, 'x');
    const auto text = format_conversation({user_message(huge)}, make_mono_theme());
    EXPECT_NE(text.find("...(truncated)"), std::string::npos);
    EXPECT_LT(text.size(), huge.size());
}

TEST(ConversationFormatTest, ColoredThemeWrapsRoleLabels)
{
    const auto theme = make_default_theme();
    const auto text = format_conversation({user_message("hi")}, theme);
    EXPECT_NE(text.find("\033[36m[you]"), std::string::npos);
}

}  // namespace
}  // namespace pi

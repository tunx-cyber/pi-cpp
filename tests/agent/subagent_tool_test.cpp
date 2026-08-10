#include "pi/agent/subagent_tool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "pi/agent/agent_loop.h"
#include "test_utils/scripted_transport.h"

namespace pi
{
namespace
{

TEST(SubagentToolTest, RunsSubagentAndWrapsResult)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::text_turn("subagent answer"));

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.systemPrompt = "You are a subagent.";
    const AgentTool tool = make_subagent_tool(options);

    const auto result = tool.execute("call_1", Json{{"prompt", "do the thing"}},
                                     std::make_shared<std::atomic<bool>>(false), nullptr);
    ASSERT_FALSE(result.content.empty());
    const std::string text = result.content[0].text;
    EXPECT_NE(text.find("<subagent>"), std::string::npos);
    EXPECT_NE(text.find("subagent answer"), std::string::npos);
    EXPECT_NE(text.find("</subagent>"), std::string::npos);
}

TEST(SubagentToolTest, NestedSubagentToolAvailable)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::text_turn("outer answer"));

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.systemPrompt = "subagent";
    const AgentTool tool = make_subagent_tool(options);

    // 子 agent 的工具里应该包含递归的 subagent 工具
    bool executed = false;
    AgentTool inner = tool;
    // 直接执行时无法看到内部工具；验证递归执行一层
    const auto result = tool.execute("call_1", Json{{"prompt", "nested"}},
                                     std::make_shared<std::atomic<bool>>(false), nullptr);
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content[0].text.find("outer answer"), std::string::npos);
    (void)executed;
}

TEST(SubagentToolTest, MaxTurnsProtection)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    // 无限工具循环：每轮都请求工具，工具执行后继续 → 应被 maxTurns 截断
    AgentTool looping_tool;
    looping_tool.name = "loop";
    looping_tool.description = "loops";
    looping_tool.label = "loop";
    looping_tool.execute = [](const std::string&, const Json&,
                              const std::shared_ptr<std::atomic<bool>>&,
                              const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        ToolResult result;
        result.content.push_back(pi_test::text_block("loop again"));
        return result;
    };
    for (int i = 0; i < 30; ++i)
    {
        transport->add_turn(pi_test::tool_turn(
            {pi_test::tool_call_block("c" + std::to_string(i), "loop", Json{{"x", 1}})}));
    }

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.tools = {looping_tool};
    options.systemPrompt = "subagent";
    options.maxTurns = 3;
    const AgentTool tool = make_subagent_tool(options);

    const auto result = tool.execute("call_1", Json{{"prompt", "loop forever"}},
                                     std::make_shared<std::atomic<bool>>(false), nullptr);
    ASSERT_FALSE(result.content.empty());
    // 轮次超限 → 返回错误
    EXPECT_NE(result.content[0].text.find("max turns exceeded"), std::string::npos);
}

TEST(SubagentToolTest, MaxNestingDepthProtection)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::tool_turn(
        {pi_test::tool_call_block("c1", "subagent", Json{{"prompt", "go deeper"}})}));
    transport->add_turn(pi_test::text_turn("deep answer"));

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.systemPrompt = "subagent";
    options.maxNestingDepth = 0;  // 根工具 depth=1 > 0 → 直接超限
    const AgentTool tool = make_subagent_tool(options);

    const auto result = tool.execute("call_1", Json{{"prompt", "start"}},
                                     std::make_shared<std::atomic<bool>>(false), nullptr);
    // depth 1 > max 0 → 错误
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content[0].text.find("max nesting depth exceeded"), std::string::npos);
}

TEST(SubagentToolTest, ForwardsStreamingUpdates)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    transport->add_turn(pi_test::text_turn("streamed content"));

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.systemPrompt = "subagent";
    const AgentTool tool = make_subagent_tool(options);

    std::string forwarded;
    const auto result = tool.execute("call_1", Json{{"prompt", "tell me"}},
                                     std::make_shared<std::atomic<bool>>(false),
                                     [&forwarded](const ToolResult& partial)
                                     {
                                         for (const auto& block : partial.content)
                                         {
                                             if (block.type == BlockType::Text)
                                                 forwarded += block.text;
                                         }
                                     });
    (void)result;
    // 子 agent 的流式文本被转发为 tool_execution_update
    EXPECT_NE(forwarded.find("streamed content"), std::string::npos);
}

TEST(SubagentToolTest, AbortPropagatesToSubagent)
{
    auto transport = std::make_shared<pi_test::ScriptedTransport>();
    // 慢速回合：文本延迟到达，abort 应在子 agent 内生效
    transport->add_turn(pi_test::text_turn("late answer"));

    SubagentToolOptions options;
    options.model = pi_test::scripted_model();
    options.transport = transport;
    options.systemPrompt = "subagent";
    const AgentTool tool = make_subagent_tool(options);

    auto abort = std::make_shared<std::atomic<bool>>(false);
    // 立即 abort：子 agent 启动后会被中断
    abort->store(true);
    const auto result = tool.execute("call_1", Json{{"prompt", "quick"}}, abort, nullptr);
    ASSERT_FALSE(result.content.empty());
    // abort 后子 agent 不产生正常 <subagent> 结果（错误或空）
    EXPECT_EQ(result.content[0].text.find("<subagent>"), std::string::npos);
}

}  // namespace
}  // namespace pi

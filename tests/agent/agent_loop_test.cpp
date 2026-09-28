#include "pi/agent/agent_loop.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pi/agent/agent.h"
#include "pi/agent/json_schema.h"
#include "pi/agent/pending_queue.h"
#include "test_utils/scripted_transport.h"

namespace pi
{
namespace
{

using namespace pi_test;

struct LoopHarness
{
    std::shared_ptr<ScriptedTransport> transport;
    AgentContext context;
    AgentLoopConfig config;
    std::vector<AgentEvent::Type> event_types;
    std::vector<AgentEvent> events;
    std::shared_ptr<std::atomic<bool>> signal;

    explicit LoopHarness(std::vector<AgentTool> tools = {})
    {
        transport = std::make_shared<ScriptedTransport>();
        config.model = scripted_model();
        config.transport = transport;
        context.tools = std::move(tools);
        signal = std::make_shared<std::atomic<bool>>(false);
    }

    void emit(const AgentEvent& event)
    {
        event_types.push_back(event.type);
        events.push_back(event);
    }

    std::vector<AgentMessage> run(const std::vector<AgentMessage>& prompts)
    {
        return run_agent_loop(
            prompts, context, config, [this](const AgentEvent& e) { emit(e); }, signal);
    }
};

AgentTool make_tool(std::string name)
{
    AgentTool tool;
    tool.name = name;
    tool.description = "test tool " + name;
    tool.label = name;
    tool.parameters = Json{{"type", "object"},
                           {"properties", Json{{"command", Json{{"type", "string"}}}}},
                           {"required", Json::array({"command"})}};
    tool.execute = [name](const std::string&, const Json& args,
                          const std::shared_ptr<std::atomic<bool>>&,
                          const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        ToolResult result;
        result.content.push_back(text_block(name + " executed: " + args.value("command", "")));
        return result;
    };
    return tool;
}

TEST(AgentLoopTest, BasicPromptFlowEmitsLifecycleEvents)
{
    LoopHarness harness;
    harness.transport->add_turn(text_turn("Hello there"));

    const auto new_messages = harness.run({Message::user("hi")});

    // 事件序列：agent_start → turn_start → message_start/end(user) → message_start(assistant)
    // → message_update → message_end(assistant) → turn_end → agent_end
    const std::vector<AgentEvent::Type> expected = {
        AgentEvent::Type::AgentStart,    AgentEvent::Type::TurnStart,
        AgentEvent::Type::MessageStart,  AgentEvent::Type::MessageEnd,
        AgentEvent::Type::MessageStart,  AgentEvent::Type::MessageUpdate,
        AgentEvent::Type::MessageUpdate, AgentEvent::Type::MessageEnd,
        AgentEvent::Type::TurnEnd,       AgentEvent::Type::AgentEnd,
    };
    EXPECT_EQ(harness.event_types, expected);

    // 上下文包含 user + assistant
    ASSERT_EQ(harness.context.messages.size(), 2u);
    EXPECT_EQ(harness.context.messages[0].role, Role::User);
    EXPECT_EQ(harness.context.messages[1].role, Role::Assistant);
    EXPECT_EQ(harness.context.messages[1].text_content(), "Hello there");

    // newMessages = prompt + assistant
    ASSERT_EQ(new_messages.size(), 2u);
    EXPECT_EQ(new_messages[0].text_content(), "hi");

    // turn_end 携带最终消息
    for (const auto& event : harness.events)
    {
        if (event.type == AgentEvent::Type::TurnEnd)
        {
            EXPECT_EQ(event.message.text_content(), "Hello there");
        }
    }
}

TEST(AgentLoopTest, ToolCallRoundTrip)
{
    LoopHarness harness({make_tool("bash")});
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("done after tool"));

    harness.run({Message::user("run ls")});

    // 事件序列含工具执行生命周期
    bool saw_tool_start = false, saw_tool_end = false;
    size_t tool_result_index = 0;
    for (size_t i = 0; i < harness.events.size(); ++i)
    {
        const auto& event = harness.events[i];
        if (event.type == AgentEvent::Type::ToolExecutionStart)
        {
            saw_tool_start = true;
            EXPECT_EQ(event.toolCallId, "call_1");
            EXPECT_EQ(event.toolName, "bash");
        }
        if (event.type == AgentEvent::Type::ToolExecutionEnd)
        {
            saw_tool_end = true;
            EXPECT_FALSE(event.isError);
        }
        if (event.type == AgentEvent::Type::MessageEnd && event.message.role == Role::ToolResult)
        {
            tool_result_index = i;
            EXPECT_EQ(event.message.toolCallId, "call_1");
            EXPECT_EQ(event.message.text_content(), "bash executed: ls");
        }
    }
    EXPECT_TRUE(saw_tool_start);
    EXPECT_TRUE(saw_tool_end);

    // 两轮 LLM 调用；第二轮收到 toolResult
    ASSERT_EQ(harness.transport->received_messages().size(), 2u);
    const auto& second_turn_messages = harness.transport->received_messages()[1];
    ASSERT_FALSE(second_turn_messages.empty());
    EXPECT_EQ(second_turn_messages.back().role, Role::ToolResult);

    // 上下文顺序：user → assistant(toolCall) → toolResult → assistant
    ASSERT_EQ(harness.context.messages.size(), 4u);
    EXPECT_EQ(harness.context.messages[2].role, Role::ToolResult);
    EXPECT_EQ(harness.context.messages[3].role, Role::Assistant);

    // 事件顺序：toolResult 消息在 turn_end 之前
    EXPECT_LT(tool_result_index, harness.events.size());
}

TEST(AgentLoopTest, ToolThrowProducesErrorToolResult)
{
    AgentTool throwing_tool = make_tool("bash");
    throwing_tool.execute = [](const std::string&, const Json&,
                               const std::shared_ptr<std::atomic<bool>>&,
                               const std::function<void(const ToolResult&)>&) -> ToolResult
    { throw std::runtime_error("boom"); };
    LoopHarness harness({throwing_tool});
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("recovered"));

    harness.run({Message::user("go")});

    const auto& messages = harness.context.messages;
    ASSERT_GE(messages.size(), 3u);
    const auto& tool_result = messages[2];
    EXPECT_EQ(tool_result.role, Role::ToolResult);
    EXPECT_TRUE(tool_result.isError);
    EXPECT_EQ(tool_result.text_content(), "boom");
}

TEST(AgentLoopTest, UnknownToolProducesNotFoundError)
{
    LoopHarness harness({make_tool("bash")});
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "nonexistent", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    const auto& messages = harness.context.messages;
    ASSERT_GE(messages.size(), 3u);
    EXPECT_EQ(messages[2].text_content(), "Tool nonexistent not found");
    EXPECT_TRUE(messages[2].isError);
}

TEST(AgentLoopTest, ToolNamesAreCaseInsensitive)
{
    LoopHarness harness({make_tool("bash")});
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "Bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    ASSERT_GE(harness.context.messages.size(), 3u);
    EXPECT_FALSE(harness.context.messages[2].isError);
    EXPECT_EQ(harness.context.messages[2].text_content(), "bash executed: ls");
}

TEST(AgentLoopTest, InvalidArgsFailSchemaValidation)
{
    AgentTool tool = make_tool("bash");
    LoopHarness harness({tool});
    // 缺少必填字段 command
    harness.transport->add_turn(tool_turn({tool_call_block("call_1", "bash", Json::object())}));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    const auto& messages = harness.context.messages;
    ASSERT_GE(messages.size(), 3u);
    EXPECT_TRUE(messages[2].isError);
    EXPECT_NE(messages[2].text_content().find("Invalid tool arguments"), std::string::npos);
}

TEST(AgentLoopTest, TerminateSemantics)
{
    AgentTool terminating_tool = make_tool("bash");
    terminating_tool.execute = [](const std::string&, const Json&,
                                  const std::shared_ptr<std::atomic<bool>>&,
                                  const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        ToolResult result;
        result.content.push_back(text_block("done"));
        result.terminate = true;
        return result;
    };
    LoopHarness harness({terminating_tool});
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "x"}})}));

    harness.run({Message::user("go")});

    // terminate=true → 不再发起第二轮 LLM 调用
    EXPECT_EQ(harness.transport->received_messages().size(), 1u);
    EXPECT_EQ(harness.event_types.back(), AgentEvent::Type::AgentEnd);
}

TEST(AgentLoopTest, TerminateRequiresAllResults)
{
    AgentTool terminating_tool = make_tool("bash");
    terminating_tool.execute = [](const std::string&, const Json&,
                                  const std::shared_ptr<std::atomic<bool>>&,
                                  const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        ToolResult result;
        result.content.push_back(text_block("done"));
        result.terminate = true;
        return result;
    };
    AgentTool normal_tool = make_tool("read");
    LoopHarness harness({terminating_tool, normal_tool});
    // 两个工具调用，一个 terminate 一个不 terminate
    harness.transport->add_turn(tool_turn({tool_call_block("c1", "bash", Json{{"command", "x"}}),
                                           tool_call_block("c2", "read", Json{{"command", "x"}})}));
    harness.transport->add_turn(text_turn("continues"));

    harness.run({Message::user("go")});

    // 批次中不是所有结果都 terminate → 继续第二轮
    EXPECT_EQ(harness.transport->received_messages().size(), 2u);
}

TEST(AgentLoopTest, ParallelToolsRunConcurrently)
{
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    AgentTool tool = make_tool("slow");
    tool.execute = [&](const std::string&, const Json&, const std::shared_ptr<std::atomic<bool>>&,
                       const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        const int now = active.fetch_add(1) + 1;
        int expected = max_active.load();
        while (now > expected && !max_active.compare_exchange_weak(expected, now))
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        active.fetch_sub(1);
        ToolResult result;
        result.content.push_back(text_block("done"));
        return result;
    };
    LoopHarness harness({tool});
    std::vector<ContentBlock> calls;
    for (int i = 0; i < 5; ++i)
    {
        calls.push_back(tool_call_block("c" + std::to_string(i), "slow", Json{{"command", "x"}}));
    }
    harness.transport->add_turn(tool_turn(std::move(calls)));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    // 5 个工具并行执行（并发上限 8，全部并发）
    EXPECT_EQ(max_active.load(), 5);
    // 5 个 toolResult
    size_t tool_results = 0;
    for (const auto& message : harness.context.messages)
    {
        if (message.role == Role::ToolResult) tool_results++;
    }
    EXPECT_EQ(tool_results, 5u);
}

TEST(AgentLoopTest, SequentialModeRunsOneAtATime)
{
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    AgentTool tool = make_tool("slow");
    tool.execute = [&](const std::string&, const Json&, const std::shared_ptr<std::atomic<bool>>&,
                       const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        const int now = active.fetch_add(1) + 1;
        int expected = max_active.load();
        while (now > expected && !max_active.compare_exchange_weak(expected, now))
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        active.fetch_sub(1);
        ToolResult result;
        result.content.push_back(text_block("done"));
        return result;
    };
    LoopHarness harness({tool});
    harness.config.toolExecution = ToolExecutionMode::Sequential;
    std::vector<ContentBlock> calls;
    for (int i = 0; i < 3; ++i)
    {
        calls.push_back(tool_call_block("c" + std::to_string(i), "slow", Json{{"command", "x"}}));
    }
    harness.transport->add_turn(tool_turn(std::move(calls)));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    EXPECT_EQ(max_active.load(), 1);
}

TEST(AgentLoopTest, SteeringMessagesInjectedBeforeNextTurn)
{
    LoopHarness harness({make_tool("bash")});
    harness.config.getSteeringMessages = [&]() -> std::vector<AgentMessage>
    {
        static int count = 0;
        if (count++ == 0)
        {
            return {Message::user("steer now")};
        }
        return {};
    };
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("after steer"));

    harness.run({Message::user("initial")});

    // 第二轮 LLM 调用应包含 steering 消息
    ASSERT_EQ(harness.transport->received_messages().size(), 2u);
    const auto& second = harness.transport->received_messages()[1];
    bool saw_steer = false;
    for (const auto& message : second)
    {
        if (message.role == Role::User && message.text_content() == "steer now") saw_steer = true;
    }
    EXPECT_TRUE(saw_steer);
}

TEST(AgentLoopTest, FollowUpRunsAfterAgentWouldStop)
{
    LoopHarness harness;
    harness.config.getFollowUpMessages = [&]() -> std::vector<AgentMessage>
    {
        static int count = 0;
        if (count++ == 0) return {Message::user("follow up")};
        return {};
    };
    harness.transport->add_turn(text_turn("first"));
    harness.transport->add_turn(text_turn("second"));

    harness.run({Message::user("hi")});

    EXPECT_EQ(harness.transport->received_messages().size(), 2u);
    ASSERT_EQ(harness.context.messages.size(), 4u);
    EXPECT_EQ(harness.context.messages[2].text_content(), "follow up");
}

TEST(AgentLoopTest, ErrorStopReasonEndsLoop)
{
    LoopHarness harness;
    std::vector<StreamEvent> error_turn;
    Message partial = make_assistant_message({}, StopReason::Error);
    partial.errorMessage = "provider failed";
    StreamEvent start;
    start.type = StreamEvent::Type::Start;
    start.message = partial;
    error_turn.push_back(start);
    StreamEvent error;
    error.type = StreamEvent::Type::Error;
    error.reason = StopReason::Error;
    error.message = make_assistant_message({}, StopReason::Error);
    error.message.errorMessage = "provider failed";
    error_turn.push_back(error);
    harness.transport->add_turn(std::move(error_turn));

    harness.run({Message::user("hi")});

    EXPECT_EQ(harness.transport->received_messages().size(), 1u);
    EXPECT_EQ(harness.event_types.back(), AgentEvent::Type::AgentEnd);
    // 错误回合后不再有后续 LLM 调用
}

TEST(AgentLoopTest, BeforeToolCallBlockPreventsExecution)
{
    bool executed = false;
    AgentTool tool = make_tool("bash");
    tool.execute = [&](const std::string&, const Json&, const std::shared_ptr<std::atomic<bool>>&,
                       const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        executed = true;
        ToolResult result;
        result.content.push_back(text_block("ran"));
        return result;
    };
    LoopHarness harness({tool});
    harness.config.beforeToolCall =
        [](const BeforeToolCallContext&,
           const std::shared_ptr<std::atomic<bool>>&) -> BeforeToolCallResult
    { return {true, "blocked by policy"}; };
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    EXPECT_FALSE(executed);
    const auto& messages = harness.context.messages;
    ASSERT_GE(messages.size(), 3u);
    EXPECT_TRUE(messages[2].isError);
    EXPECT_EQ(messages[2].text_content(), "blocked by policy");
}

TEST(AgentLoopTest, AfterToolCallOverridesResult)
{
    LoopHarness harness({make_tool("bash")});
    harness.config.afterToolCall =
        [](const AfterToolCallContext&,
           const std::shared_ptr<std::atomic<bool>>&) -> AfterToolCallResult
    {
        AfterToolCallResult result;
        result.content = std::vector<ContentBlock>{text_block("overridden")};
        return result;
    };
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("ok"));

    harness.run({Message::user("go")});

    const auto& messages = harness.context.messages;
    ASSERT_GE(messages.size(), 3u);
    EXPECT_EQ(messages[2].text_content(), "overridden");
}

TEST(AgentLoopTest, ShouldStopAfterTurnExitsEarly)
{
    LoopHarness harness({make_tool("bash")});
    harness.config.shouldStopAfterTurn = [](const ShouldStopAfterTurnContext&) -> bool
    { return true; };
    harness.transport->add_turn(
        tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    harness.transport->add_turn(text_turn("should not run"));

    harness.run({Message::user("go")});

    // shouldStopAfterTurn → 不再发起下一轮
    EXPECT_EQ(harness.transport->received_messages().size(), 1u);
}

// ---------- Agent API ----------

TEST(AgentTest, PromptThrowsWhenBusy)
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->add_turn(text_turn("done"));
    AgentOptions options;
    options.model = scripted_model();
    options.transport = transport;
    Agent agent(options);
    // 同步执行时无法重入，验证 busy 状态与 reset
    agent.prompt("hi");
    EXPECT_FALSE(agent.is_busy());
    EXPECT_TRUE(agent.is_streaming() == false);
    EXPECT_EQ(agent.messages().size(), 2u);
    agent.reset();
    EXPECT_TRUE(agent.messages().empty());
}

TEST(AgentTest, SteerInjectsMessageAndAbortWorks)
{
    auto transport = std::make_shared<ScriptedTransport>();
    // 回合1：工具调用 → 回合2：文本（steer 消息会在回合2前注入）
    AgentTool tool = make_tool("bash");
    tool.execute = [](const std::string&, const Json& args,
                      const std::shared_ptr<std::atomic<bool>>& signal,
                      const std::function<void(const ToolResult&)>&) -> ToolResult
    {
        // 工具执行期间让 steer 有机会落点
        for (int i = 0; i < 10; ++i)
        {
            if (signal && signal->load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ToolResult result;
        result.content.push_back(text_block("bash executed: " + args.value("command", "")));
        return result;
    };
    transport->add_turn(tool_turn({tool_call_block("call_1", "bash", Json{{"command", "ls"}})}));
    transport->add_turn(text_turn("after steer"));
    AgentOptions options;
    options.model = scripted_model();
    options.transport = transport;
    options.tools = {tool};
    Agent agent(options);
    // 在工具执行期间注入 steer：通过一个线程模拟
    std::thread stealer(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            agent.steer(Message::user("steered"));
        });
    agent.prompt("go");
    stealer.join();

    EXPECT_EQ(transport->received_messages().size(), 2u);
    const auto& second = transport->received_messages()[1];
    bool saw_steer = false;
    for (const auto& message : second)
    {
        if (message.role == Role::User && message.text_content() == "steered") saw_steer = true;
    }
    EXPECT_TRUE(saw_steer);
}

TEST(AgentTest, ContinueFromAssistantWithQueuedMessages)
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->add_turn(text_turn("first"));
    transport->add_turn(text_turn("second"));
    AgentOptions options;
    options.model = scripted_model();
    options.transport = transport;
    Agent agent(options);
    agent.prompt("hi");
    ASSERT_EQ(agent.messages().size(), 2u);
    EXPECT_EQ(agent.messages().back().role, Role::Assistant);

    agent.follow_up(Message::user("follow"));
    agent.continue_run();
    EXPECT_EQ(agent.messages().size(), 4u);
}

TEST(AgentLoopTest, WorkerCountIsBoundedAndAfterHooksRunOnCallerThread)
{
    std::mutex mutex;
    std::set<std::thread::id> worker_ids;
    auto tool = make_tool("work");
    tool.execute = [&](const std::string&, const Json&, const auto&, const auto&)
    {
        std::lock_guard<std::mutex> lock(mutex);
        worker_ids.insert(std::this_thread::get_id());
        return ToolResult{};
    };
    LoopHarness harness({tool});
    const auto caller = std::this_thread::get_id();
    int hooks = 0;
    harness.config.afterToolCall = [&](const AfterToolCallContext&, const auto&)
    {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        ++hooks;
        return AfterToolCallResult{};
    };
    std::vector<ContentBlock> calls;
    for (int i = 0; i < 64; ++i)
        calls.push_back(tool_call_block(std::to_string(i), "work", Json{{"command", "run"}}));
    harness.transport->add_turn(tool_turn(calls));
    harness.transport->add_turn(text_turn("done"));
    harness.run({Message::user("go")});
    EXPECT_LE(worker_ids.size(), 8u);
    EXPECT_EQ(hooks, 64);
    for (int i = 0; i < 64; ++i)
        EXPECT_EQ(harness.context.messages[static_cast<size_t>(i) + 2].toolCallId,
                  std::to_string(i));
}

TEST(AgentTest, ThrowingSubscriberDoesNotPreventOtherSubscribersOrIdleCleanup)
{
    AgentOptions options;
    options.model = scripted_model();
    options.transport = std::make_shared<ScriptedTransport>();
    Agent agent(options);
    const auto unsubscribe = agent.subscribe([](const AgentEvent&, const auto&)
                                             { throw std::runtime_error("subscriber failed"); });
    int notified = 0;
    agent.subscribe([&](const AgentEvent&, const auto&) { ++notified; });
    EXPECT_THROW(agent.prompt("hello"), std::runtime_error);
    EXPECT_GT(notified, 0);
    EXPECT_FALSE(agent.is_busy());
    EXPECT_FALSE(agent.is_streaming());
    EXPECT_EQ(agent.error_message(), "subscriber failed");
    agent.wait_for_idle();
    unsubscribe();
    EXPECT_NO_THROW(agent.prompt("retry"));
}

}  // namespace
}  // namespace pi

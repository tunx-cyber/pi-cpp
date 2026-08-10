#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pi/ai/cost.h"
#include "pi/ai/events.h"
#include "pi/ai/model_registry.h"
#include "pi/ai/openai_transport.h"
#include "test_utils/fake_sse_server.h"

namespace pi
{
namespace
{

using EventList = std::vector<StreamEvent>;

struct EventCollector
{
    std::mutex mutex;
    std::condition_variable cv;
    EventList events;
    bool done = false;

    void push(const StreamEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
            if (event.type == StreamEvent::Type::Done || event.type == StreamEvent::Type::Error)
                done = true;
        }
        cv.notify_all();
    }

    EventList wait_for_end(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, timeout, [this] { return done; });
        return events;
    }
};

std::string json_delta(std::string content)
{
    return std::string(
               "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
               "\"content\":\"") +
           content + "\"},\"finish_reason\":null}]}";
}

class TransportTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        server_ = std::make_unique<pi_test::FakeSseServer>();
        model_ = *get_model("deepseek-v4-flash");
    }

    std::shared_ptr<TransportAdapter> make_transport()
    {
        return make_openai_completions_transport(server_->base_url(), "test-key",
                                                 std::chrono::milliseconds(5000));
    }

    std::unique_ptr<pi_test::FakeSseServer> server_;
    ModelInfo model_;
};

TEST_F(TransportTest, StreamsTextAndParsesUsage)
{
    server_->set_script({
        pi_test::sse_data(json_delta("Hello")),
        pi_test::sse_data("{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\" "
                          "world\"},\"finish_reason\":null}]}"),
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,"
            "\"prompt_tokens_details\":{\"cached_tokens\":2,\"cache_write_tokens\":1}}}"),
        pi_test::sse_data("[DONE]"),
    });

    EventCollector collector;
    auto transport = make_transport();
    StreamRequestOptions opts;
    opts.systemPrompt = "You are helpful.";
    transport->stream_chat(model_, {Message::user("hi")}, opts,
                           [&](const StreamEvent& e) { collector.push(e); });

    const auto events = collector.wait_for_end();
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.back().type, StreamEvent::Type::Done);

    // 事件序列：start → text_start → text_delta x2 → text_end → done
    ASSERT_GE(events.size(), 5u);
    EXPECT_EQ(events[0].type, StreamEvent::Type::Start);
    EXPECT_EQ(events[1].type, StreamEvent::Type::TextStart);
    EXPECT_EQ(events[2].type, StreamEvent::Type::TextDelta);
    EXPECT_EQ(events[2].delta, "Hello");
    EXPECT_EQ(events[3].type, StreamEvent::Type::TextDelta);
    EXPECT_EQ(events[3].delta, " world");
    EXPECT_EQ(events[4].type, StreamEvent::Type::TextEnd);
    EXPECT_EQ(events[4].content, "Hello world");

    const auto& done = events.back();
    EXPECT_EQ(done.reason, StopReason::Stop);
    EXPECT_EQ(done.message.role, Role::Assistant);
    EXPECT_EQ(done.message.model, "deepseek-v4-flash");
    EXPECT_EQ(done.message.text_content(), "Hello world");
    // usage: input = 10 - 2 - 1 = 7
    EXPECT_EQ(done.message.usage.input, 7);
    EXPECT_EQ(done.message.usage.output, 5);
    EXPECT_EQ(done.message.usage.cacheRead, 2);
    EXPECT_EQ(done.message.usage.cacheWrite, 1);
    EXPECT_EQ(done.message.usage.totalTokens, 15);
    EXPECT_NEAR(done.message.usage.cost.total, calculate_cost(model_, done.message.usage).total,
                1e-12);

    // 请求体断言：model / stream_options.include_usage / system role
    const Json body = Json::parse(server_->last_request_body());
    EXPECT_EQ(body["model"], "deepseek-v4-flash");
    EXPECT_EQ(body["stream_options"]["include_usage"], true);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][0]["content"], "You are helpful.");
    EXPECT_EQ(body["messages"][1]["role"], "user");
}

TEST_F(TransportTest, SendsThinkingParamAndReasoningEffort)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}"),
    });
    auto transport = make_transport();
    EventCollector collector;
    StreamRequestOptions opts;
    opts.reasoning = ThinkingLevel::High;
    transport->stream_chat(model_, {Message::user("hi")}, opts,
                           [&](const StreamEvent& e) { collector.push(e); });
    collector.wait_for_end();

    const Json body = Json::parse(server_->last_request_body());
    // deepseek thinkingFormat：thinking + reasoning_effort（thinkingLevelMap high→high）
    EXPECT_EQ(body["thinking"]["type"], "enabled");
    EXPECT_EQ(body["reasoning_effort"], "high");
}

TEST_F(TransportTest, SendsThinkingDisabledWhenOff)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"),
    });
    auto transport = make_transport();
    EventCollector collector;
    StreamRequestOptions opts;
    opts.reasoning = ThinkingLevel::Off;
    transport->stream_chat(model_, {Message::user("hi")}, opts,
                           [&](const StreamEvent& e) { collector.push(e); });
    collector.wait_for_end();

    const Json body = Json::parse(server_->last_request_body());
    EXPECT_EQ(body["thinking"]["type"], "disabled");
    EXPECT_FALSE(body.contains("reasoning_effort"));
}

TEST_F(TransportTest, StreamsThinkingBlocks)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Let me\"},"
            "\"finish_reason\":null}]}"),
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\" think\"},"
            "\"finish_reason\":null}]}"),
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Answer\"},"
            "\"finish_reason\":\"stop\"}]}"),
        pi_test::sse_data("[DONE]"),
    });
    auto transport = make_transport();
    EventCollector collector;
    transport->stream_chat(model_, {Message::user("hi")}, {},
                           [&](const StreamEvent& e) { collector.push(e); });
    const auto events = collector.wait_for_end();
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.back().type, StreamEvent::Type::Done);

    bool saw_thinking_start = false, saw_thinking_delta = false, saw_thinking_end = false;
    for (const auto& event : events)
    {
        if (event.type == StreamEvent::Type::ThinkingStart)
        {
            saw_thinking_start = true;
            EXPECT_EQ(event.contentIndex, 0);
        }
        if (event.type == StreamEvent::Type::ThinkingDelta) saw_thinking_delta = true;
        if (event.type == StreamEvent::Type::ThinkingEnd)
        {
            saw_thinking_end = true;
            EXPECT_EQ(event.content, "Let me think");
        }
    }
    EXPECT_TRUE(saw_thinking_start);
    EXPECT_TRUE(saw_thinking_delta);
    EXPECT_TRUE(saw_thinking_end);

    const auto& done = events.back();
    ASSERT_EQ(done.message.content.size(), 2u);
    EXPECT_EQ(done.message.content[0].type, BlockType::Thinking);
    EXPECT_EQ(done.message.content[0].thinking, "Let me think");
    EXPECT_EQ(done.message.content[0].thinkingSignature, "reasoning_content");
    EXPECT_EQ(done.message.content[1].type, BlockType::Text);
    EXPECT_EQ(done.message.content[1].text, "Answer");
}

TEST_F(TransportTest, StreamsToolCallsWithDeltaAccumulation)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
            "{\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"bash\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}"),
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
            "{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\\\"ls\"}}]},"
            "\"finish_reason\":null}]}"),
        pi_test::sse_data("{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
                          "{\"index\":0,\"function\":{\"arguments\":\" -la\\\"}\"}}]},"
                          "\"finish_reason\":\"tool_calls\"}]}"),
        pi_test::sse_data("[DONE]"),
    });
    auto transport = make_transport();
    EventCollector collector;
    transport->stream_chat(model_, {Message::user("list")}, {},
                           [&](const StreamEvent& e) { collector.push(e); });
    const auto events = collector.wait_for_end();
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.back().type, StreamEvent::Type::Done);
    EXPECT_EQ(events.back().reason, StopReason::ToolUse);

    bool saw_start = false, saw_end = false;
    for (const auto& event : events)
    {
        if (event.type == StreamEvent::Type::ToolCallStart)
        {
            saw_start = true;
            EXPECT_EQ(event.message.content[0].id, "call_1");
        }
        if (event.type == StreamEvent::Type::ToolCallEnd)
        {
            saw_end = true;
            EXPECT_EQ(event.toolCall.name, "bash");
            EXPECT_EQ(event.toolCall.arguments["command"], "ls -la");
        }
    }
    EXPECT_TRUE(saw_start);
    EXPECT_TRUE(saw_end);

    const auto& done = events.back();
    ASSERT_EQ(done.message.content.size(), 1u);
    EXPECT_EQ(done.message.content[0].type, BlockType::ToolCall);
    EXPECT_EQ(done.message.content[0].name, "bash");
    EXPECT_EQ(done.message.content[0].arguments["command"], "ls -la");
}

TEST_F(TransportTest, AbortsMidStreamWithAbortedStopReason)
{
    server_->set_script(
        {
            pi_test::sse_data(json_delta("Hello")),
            pi_test::sse_data(json_delta(" world")),
            pi_test::sse_data("{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_"
                              "reason\":\"stop\"}]}"),
        },
        std::chrono::milliseconds(150));

    auto transport = make_transport();
    auto abort = std::make_shared<std::atomic<bool>>(false);
    EventCollector collector;
    StreamRequestOptions opts;
    opts.abort = abort;
    transport->stream_chat(model_, {Message::user("hi")}, opts,
                           [&](const StreamEvent& e)
                           {
                               collector.push(e);
                               if (e.type == StreamEvent::Type::TextDelta) abort->store(true);
                           });
    const auto events = collector.wait_for_end();
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.back().type, StreamEvent::Type::Error);
    EXPECT_EQ(events.back().reason, StopReason::Aborted);
    EXPECT_EQ(events.back().message.stopReason, StopReason::Aborted);
    EXPECT_NE(events.back().message.errorMessage.find("abort"), std::string::npos);
}

TEST_F(TransportTest, ReturnsErrorEventOnHttpError)
{
    server_->set_handler([](const std::string&, const std::map<std::string, std::string>&)
                         { return "{\"error\":{\"message\":\"bad request\"}}"; });
    server_->set_status(400);

    auto transport = make_transport();
    EventCollector collector;
    StreamRequestOptions opts;
    transport->stream_chat(model_, {Message::user("hi")}, opts,
                           [&](const StreamEvent& e) { collector.push(e); });
    const auto events = collector.wait_for_end();
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.back().type, StreamEvent::Type::Error);
    EXPECT_EQ(events.back().reason, StopReason::Error);
    EXPECT_EQ(events[0].type, StreamEvent::Type::Start);
    EXPECT_NE(events.back().message.errorMessage.find("bad request"), std::string::npos);
}

TEST_F(TransportTest, CompleteChatAggregatesMessage)
{
    server_->set_script({
        pi_test::sse_data(json_delta("Hello")),
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3}}"),
        pi_test::sse_data("[DONE]"),
    });
    auto transport = make_transport();
    const Message result = transport->complete_chat(model_, {Message::user("hi")}, {});
    EXPECT_EQ(result.stopReason, StopReason::Stop);
    EXPECT_EQ(result.text_content(), "Hello");
    EXPECT_EQ(result.usage.output, 3);
}

TEST_F(TransportTest, ReplaysAssistantThinkingAndToolCalls)
{
    // assistant 消息带 thinking + tool call → 请求体应包含 reasoning_content 与 content: null
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"),
    });

    Message assistant;
    assistant.role = Role::Assistant;
    assistant.model = "deepseek-v4-flash";
    assistant.provider = "deepseek";
    assistant.api = "openai-completions";
    assistant.stopReason = StopReason::ToolUse;
    ContentBlock thinking;
    thinking.type = BlockType::Thinking;
    thinking.thinking = "hidden reasoning";
    thinking.thinkingSignature = "reasoning_content";
    ContentBlock call;
    call.type = BlockType::ToolCall;
    call.id = "call_1";
    call.name = "bash";
    call.arguments = Json{{"command", "ls"}};
    assistant.content = {thinking, call};

    Message result_msg = Message::toolResult("call_1", "bash", "output", false);

    auto transport = make_transport();
    EventCollector collector;
    StreamRequestOptions opts;
    transport->stream_chat(model_, {Message::user("hi"), assistant, result_msg}, opts,
                           [&](const StreamEvent& e) { collector.push(e); });
    collector.wait_for_end();

    const Json body = Json::parse(server_->last_request_body());
    const auto& messages = body["messages"];
    ASSERT_GE(messages.size(), 3u);
    const auto& assistant_wire = messages[1];
    EXPECT_EQ(assistant_wire["role"], "assistant");
    // deepseek requiresReasoningContentOnAssistantMessages：带 reasoning_content
    EXPECT_EQ(assistant_wire["reasoning_content"], "hidden reasoning");
    // 无文本 → content null
    EXPECT_TRUE(assistant_wire["content"].is_null());
    EXPECT_EQ(assistant_wire["tool_calls"][0]["id"], "call_1");
    EXPECT_EQ(assistant_wire["tool_calls"][0]["function"]["name"], "bash");
    EXPECT_EQ(assistant_wire["tool_calls"][0]["function"]["arguments"], "{\"command\":\"ls\"}");
    // toolResult → role tool
    const auto& tool_wire = messages[2];
    EXPECT_EQ(tool_wire["role"], "tool");
    EXPECT_EQ(tool_wire["tool_call_id"], "call_1");
    EXPECT_EQ(tool_wire["content"], "output");
}

TEST_F(TransportTest, DowngradesImagesForNonVisionModel)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"),
    });
    auto transport = make_transport();
    EventCollector collector;
    Message user;
    user.role = Role::User;
    ContentBlock image;
    image.type = BlockType::Image;
    image.data = "AAAA";
    image.mimeType = "image/png";
    user.content = {image};
    transport->stream_chat(model_, {user}, {}, [&](const StreamEvent& e) { collector.push(e); });
    collector.wait_for_end();

    const Json body = Json::parse(server_->last_request_body());
    // deepseek-v4-flash 不支持图片 → 降级为占位文本
    EXPECT_EQ(body["messages"][0]["content"], "(image omitted: model does not support images)");
}

TEST_F(TransportTest, ExactEventSequenceMixedContent)
{
    // 录制 fixture：thinking → tool call 参数流 → 文本 → done
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"s1\",\"model\":\"deepseek-v4-flash\",\"choices\":[{\"index\":0,"
            "\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"Plan\"},\"finish_reason\":"
            "null}]}"),
        pi_test::sse_data("{\"id\":\"s1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
                          "{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"bash\","
                          "\"arguments\":\"{\\\"a\\\":1}\"}}]},"
                          "\"finish_reason\":null}]}"),
        pi_test::sse_data(
            "{\"id\":\"s1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Final\"},"
            "\"finish_reason\":null}]}"),
        pi_test::sse_data("{\"id\":\"s1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
                          "\"tool_calls\"}]}"),
        pi_test::sse_data("[DONE]"),
    });
    auto transport = make_transport();
    EventCollector collector;
    transport->stream_chat(model_, {Message::user("go")}, {},
                           [&](const StreamEvent& e) { collector.push(e); });
    const auto events = collector.wait_for_end();

    // 精确断言事件类型序列
    std::vector<StreamEvent::Type> types;
    for (const auto& event : events) types.push_back(event.type);
    // finish 按 block 顺序：thinking → toolCall → text（与 pi 的 finishBlock 一致）
    const std::vector<StreamEvent::Type> expected = {
        StreamEvent::Type::Start,         StreamEvent::Type::ThinkingStart,
        StreamEvent::Type::ThinkingDelta, StreamEvent::Type::ToolCallStart,
        StreamEvent::Type::ToolCallDelta, StreamEvent::Type::TextStart,
        StreamEvent::Type::TextDelta,     StreamEvent::Type::ThinkingEnd,
        StreamEvent::Type::ToolCallEnd,   StreamEvent::Type::TextEnd,
        StreamEvent::Type::Done,
    };
    ASSERT_EQ(types, expected);

    const auto& done = events.back();
    EXPECT_EQ(done.message.content.size(), 3u);
    EXPECT_EQ(done.message.content[0].type, BlockType::Thinking);
    EXPECT_EQ(done.message.content[1].type, BlockType::ToolCall);
    EXPECT_EQ(done.message.content[1].arguments["a"], 1);
    EXPECT_EQ(done.message.content[2].type, BlockType::Text);
    EXPECT_EQ(done.message.stopReason, StopReason::ToolUse);
}

TEST_F(TransportTest, InsertsSyntheticToolResultForOrphanedCalls)
{
    server_->set_script({
        pi_test::sse_data(
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"),
    });
    auto transport = make_transport();
    EventCollector collector;
    Message assistant;
    assistant.role = Role::Assistant;
    assistant.model = "deepseek-v4-flash";
    assistant.provider = "deepseek";
    assistant.api = "openai-completions";
    assistant.stopReason = StopReason::ToolUse;
    ContentBlock call;
    call.type = BlockType::ToolCall;
    call.id = "orphan_1";
    call.name = "bash";
    call.arguments = Json::object();
    assistant.content = {call};
    // 消息序列以 user 结尾（孤儿 tool call 无对应 toolResult）
    transport->stream_chat(model_, {assistant, Message::user("next")}, {},
                           [&](const StreamEvent& e) { collector.push(e); });
    collector.wait_for_end();

    const Json body = Json::parse(server_->last_request_body());
    const auto& messages = body["messages"];
    ASSERT_GE(messages.size(), 2u);
    const auto& tool_wire = messages[1];
    EXPECT_EQ(tool_wire["role"], "tool");
    EXPECT_EQ(tool_wire["tool_call_id"], "orphan_1");
    EXPECT_EQ(tool_wire["content"], "No result provided");
}

}  // namespace
}  // namespace pi

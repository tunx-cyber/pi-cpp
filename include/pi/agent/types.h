#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pi/ai/events.h"
#include "pi/ai/model_info.h"
#include "pi/ai/types.h"

namespace pi
{

// AgentMessage 与 ai 层 Message 同构（pi 的 custom 消息类型裁剪掉）
using AgentMessage = Message;

enum class QueueMode
{
    All,
    OneAtATime
};

enum class ToolExecutionMode
{
    Sequential,
    Parallel
};

/** 工具执行结果，镜像 pi 的 AgentToolResult。 */
struct ToolResult
{
    std::vector<ContentBlock> content;
    Json details = Json::object();
    bool terminate = false;
};

struct ToolResultMessage
{
    Message message;
};

/**
 * 工具定义，镜像 pi 的 AgentTool。
 * execute 抛异常 = error toolResult（pi 契约）。
 */
struct AgentTool
{
    std::string name;
    std::string description;
    std::string label;
    Json parameters = Json::object();  // JSON Schema
    ToolExecutionMode executionMode = ToolExecutionMode::Parallel;
    /** 兼容 shim：原始参数 → schema 匹配参数（可选）。 */
    std::function<Json(const Json&)> prepareArguments;
    /**
     * execute(toolCallId, args, abort, onUpdate) → ToolResult。
     * onUpdate 用于流式部分结果（tool_execution_update）。
     */
    std::function<ToolResult(const std::string&, const Json&,
                             const std::shared_ptr<std::atomic<bool>>&,
                             const std::function<void(const ToolResult&)>&)>
        execute;
};

/** agent 循环事件，镜像 pi 的 AgentEvent。 */
struct AgentEvent
{
    enum class Type
    {
        AgentStart,
        AgentEnd,
        TurnStart,
        TurnEnd,
        MessageStart,
        MessageUpdate,
        MessageEnd,
        ToolExecutionStart,
        ToolExecutionUpdate,
        ToolExecutionEnd,
    };

    Type type = Type::AgentStart;

    // agent_end
    std::vector<AgentMessage> messages;

    // turn_end / message_*
    AgentMessage message;
    std::vector<Message> toolResults;  // turn_end 的 toolResult 消息

    // message_update
    std::optional<StreamEvent> assistantMessageEvent;

    // tool_execution_*
    std::string toolCallId;
    std::string toolName;
    Json args = Json::object();
    bool isError = false;
    ToolResult result;
    ToolResult partialResult;
};

using AgentEventSink = std::function<void(const AgentEvent&)>;

/** beforeToolCall 返回；block=true 阻止执行。 */
struct BeforeToolCallResult
{
    bool block = false;
    std::string reason;
};

/** afterToolCall 返回的字段级覆盖。 */
struct AfterToolCallResult
{
    bool hasContent = false;
    std::vector<ContentBlock> content;
    bool hasDetails = false;
    Json details;
    bool hasIsError = false;
    bool isError = false;
    bool hasTerminate = false;
    bool terminate = false;
};

struct AgentContext
{
    std::string systemPrompt;
    std::vector<AgentMessage> messages;
    std::vector<AgentTool> tools;
};

struct BeforeToolCallContext
{
    const AgentMessage* assistantMessage;
    const ContentBlock* toolCall;
    Json args;
    AgentContext* context;
};

struct AfterToolCallContext
{
    const AgentMessage* assistantMessage;
    const ContentBlock* toolCall;
    Json args;
    ToolResult result;
    bool isError = false;
    AgentContext* context;
};

struct ShouldStopAfterTurnContext
{
    const AgentMessage* message;
    const std::vector<Message>* toolResults;
    AgentContext* context;
    const std::vector<AgentMessage>* newMessages;
};

struct AgentLoopTurnUpdate
{
    std::optional<AgentContext> context;
    std::optional<ModelInfo> model;
    std::optional<ThinkingLevel> thinkingLevel;
};

}  // namespace pi

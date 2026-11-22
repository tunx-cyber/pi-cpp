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

/**
 * 工具定义，镜像 pi 的 AgentTool。
 * execute 抛异常 = error toolResult（pi 契约）。
 */
struct AgentTool
{
    std::string name;
    std::string description;
    Json parameters = Json::object();  // JSON Schema
    ToolExecutionMode executionMode = ToolExecutionMode::Parallel;
    /** execute(toolCallId, args, abort) → ToolResult；抛异常 = error toolResult。 */
    std::function<ToolResult(const std::string&, const Json&,
                             const std::shared_ptr<std::atomic<bool>>&)>
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
        ToolExecutionEnd,
    };

    Type type = Type::AgentStart;

    // turn_end / message_*
    AgentMessage message;

    // message_update
    std::optional<StreamEvent> assistantMessageEvent;

    // tool_execution_*
    std::string toolCallId;
    std::string toolName;
    Json args = Json::object();
    bool isError = false;
    ToolResult result;
};

using AgentEventSink = std::function<void(const AgentEvent&)>;

struct AgentContext
{
    std::string systemPrompt;
    std::vector<AgentMessage> messages;
    std::vector<AgentTool> tools;
};

struct ShouldStopAfterTurnContext
{
    const AgentMessage* message;
    const std::vector<Message>* toolResults;
    const AgentContext* context;
    const std::vector<AgentMessage>* newMessages;
};

}  // namespace pi

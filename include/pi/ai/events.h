#pragma once

#include <string>

#include "pi/ai/types.h"

namespace pi
{

/**
 * Event protocol for stream_chat, mirroring pi's AssistantMessageEvent.
 * Every event except start carries a `message` field: the partial assistant
 * message for start/delta/end events, the final message for done/error.
 */
struct StreamEvent
{
    enum class Type
    {
        Start,
        TextStart,
        TextDelta,
        TextEnd,
        ThinkingStart,
        ThinkingDelta,
        ThinkingEnd,
        ToolCallStart,
        ToolCallDelta,
        ToolCallEnd,
        Done,
        Error,
    };

    Type type = Type::Start;
    int contentIndex = -1;
    std::string delta;                     // text_delta / thinking_delta / toolcall_delta
    std::string content;                   // text_end / thinking_end full content
    ContentBlock toolCall;                 // toolcall_end
    StopReason reason = StopReason::Stop;  // done / error
    Message message;
};

}  // namespace pi

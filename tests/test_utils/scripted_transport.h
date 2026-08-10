#pragma once

// 脚本化假 transport：按调用顺序回放脚本回合（agent 层测试共用）。

#include <memory>
#include <string>
#include <vector>

#include "pi/ai/events.h"
#include "pi/ai/transport_adapter.h"

namespace pi_test
{

class ScriptedTransport : public pi::TransportAdapter
{
   public:
    struct ScriptTurn
    {
        std::vector<pi::StreamEvent> events;
    };

    void add_turn(std::vector<pi::StreamEvent> events) { turns_.push_back({std::move(events)}); }

    void stream_chat(const pi::ModelInfo&, const std::vector<pi::Message>& messages,
                     const pi::StreamRequestOptions&,
                     const std::function<void(const pi::StreamEvent&)>& sink) override
    {
        received_messages_.push_back(messages);
        if (turn_index_ < turns_.size())
        {
            for (const auto& event : turns_[turn_index_].events) sink(event);
            turn_index_++;
            return;
        }
        pi::Message output;
        output.role = pi::Role::Assistant;
        output.model = "scripted";
        output.stopReason = pi::StopReason::Stop;
        pi::StreamEvent start;
        start.type = pi::StreamEvent::Type::Start;
        start.message = output;
        sink(start);
        pi::StreamEvent done;
        done.type = pi::StreamEvent::Type::Done;
        done.reason = pi::StopReason::Stop;
        done.message = output;
        sink(done);
    }

    const std::vector<std::vector<pi::Message>>& received_messages() const
    {
        return received_messages_;
    }

   private:
    std::vector<ScriptTurn> turns_;
    size_t turn_index_ = 0;
    std::vector<std::vector<pi::Message>> received_messages_;
};

inline pi::Message make_assistant_message(std::vector<pi::ContentBlock> content,
                                          pi::StopReason reason)
{
    pi::Message message;
    message.role = pi::Role::Assistant;
    message.model = "scripted";
    message.stopReason = reason;
    message.content = std::move(content);
    return message;
}

inline pi::ContentBlock text_block(std::string text)
{
    pi::ContentBlock block;
    block.type = pi::BlockType::Text;
    block.text = std::move(text);
    return block;
}

inline pi::ContentBlock tool_call_block(std::string id, std::string name, pi::Json args)
{
    pi::ContentBlock block;
    block.type = pi::BlockType::ToolCall;
    block.id = std::move(id);
    block.name = std::move(name);
    block.arguments = std::move(args);
    return block;
}

/** 文本回合：start → text_start/delta → done。 */
inline std::vector<pi::StreamEvent> text_turn(const std::string& text,
                                              pi::StopReason reason = pi::StopReason::Stop)
{
    std::vector<pi::StreamEvent> events;
    pi::StreamEvent start;
    start.type = pi::StreamEvent::Type::Start;
    start.message = make_assistant_message({}, reason);
    events.push_back(start);

    pi::StreamEvent text_start;
    text_start.type = pi::StreamEvent::Type::TextStart;
    text_start.contentIndex = 0;
    text_start.message = make_assistant_message({text_block("")}, reason);
    events.push_back(text_start);

    pi::StreamEvent delta;
    delta.type = pi::StreamEvent::Type::TextDelta;
    delta.contentIndex = 0;
    delta.delta = text;
    delta.message = make_assistant_message({text_block(text)}, reason);
    events.push_back(delta);

    pi::StreamEvent done;
    done.type = pi::StreamEvent::Type::Done;
    done.reason = reason;
    done.message = make_assistant_message({text_block(text)}, reason);
    events.push_back(done);
    return events;
}

/** 工具调用回合：start → done（最终消息带 toolCall）。 */
inline std::vector<pi::StreamEvent> tool_turn(std::vector<pi::ContentBlock> tool_calls)
{
    std::vector<pi::StreamEvent> events;
    pi::StreamEvent start;
    start.type = pi::StreamEvent::Type::Start;
    start.message = make_assistant_message({}, pi::StopReason::ToolUse);
    events.push_back(start);
    pi::StreamEvent done;
    done.type = pi::StreamEvent::Type::Done;
    done.reason = pi::StopReason::ToolUse;
    done.message = make_assistant_message(std::move(tool_calls), pi::StopReason::ToolUse);
    events.push_back(done);
    return events;
}

inline pi::ModelInfo scripted_model()
{
    pi::ModelInfo model;
    model.id = "scripted";
    model.api = "openai-completions";
    model.provider = "scripted";
    model.baseUrl = "http://localhost";
    model.costInput = 1;
    model.costOutput = 2;
    return model;
}

}  // namespace pi_test

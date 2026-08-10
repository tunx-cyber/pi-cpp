#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pi
{

using Json = nlohmann::json;

enum class Role
{
    User,
    Assistant,
    ToolResult
};

enum class BlockType
{
    Text,
    Thinking,
    Image,
    ToolCall
};

enum class StopReason
{
    Stop,
    Length,
    ToolUse,
    Error,
    Aborted
};

enum class ThinkingLevel
{
    Off,
    Minimal,
    Low,
    Medium,
    High,
    Xhigh
};

enum class CacheRetention
{
    None,
    Short,
    Long
};

inline const char* to_string(ThinkingLevel level)
{
    switch (level)
    {
        case ThinkingLevel::Off:
            return "off";
        case ThinkingLevel::Minimal:
            return "minimal";
        case ThinkingLevel::Low:
            return "low";
        case ThinkingLevel::Medium:
            return "medium";
        case ThinkingLevel::High:
            return "high";
        case ThinkingLevel::Xhigh:
            return "xhigh";
    }
    return "off";
}

inline std::optional<ThinkingLevel> thinking_level_from_string(const std::string& s)
{
    if (s == "off") return ThinkingLevel::Off;
    if (s == "minimal") return ThinkingLevel::Minimal;
    if (s == "low") return ThinkingLevel::Low;
    if (s == "medium") return ThinkingLevel::Medium;
    if (s == "high") return ThinkingLevel::High;
    if (s == "xhigh") return ThinkingLevel::Xhigh;
    return std::nullopt;
}

inline const char* to_string(StopReason reason)
{
    switch (reason)
    {
        case StopReason::Stop:
            return "stop";
        case StopReason::Length:
            return "length";
        case StopReason::ToolUse:
            return "toolUse";
        case StopReason::Error:
            return "error";
        case StopReason::Aborted:
            return "aborted";
    }
    return "stop";
}

inline const char* to_string(Role role)
{
    switch (role)
    {
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
        case Role::ToolResult:
            return "toolResult";
    }
    return "user";
}

/** Content block mirroring pi's TextContent/ThinkingContent/ImageContent/ToolCall. */
struct ContentBlock
{
    BlockType type = BlockType::Text;

    // text / thinking
    std::string text;
    std::string thinking;
    std::string thinkingSignature;
    bool redacted = false;

    // image
    std::string data;  // base64
    std::string mimeType;

    // toolCall
    std::string id;
    std::string name;
    Json arguments = Json::object();
    std::string thoughtSignature;
};

/** Cost in USD, mirroring pi's Usage.cost. */
struct Cost
{
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
    double total = 0;
};

/** Token usage mirroring pi's Usage. */
struct Usage
{
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite = 0;
    int64_t totalTokens = 0;
    Cost cost;
};

/**
 * Unified message mirroring pi's UserMessage/AssistantMessage/ToolResultMessage.
 * User/toolResult content is stored as blocks (text/image); a plain string user
 * message is normalized to a single text block.
 */
struct Message
{
    Role role = Role::User;
    std::vector<ContentBlock> content;
    int64_t timestamp = 0;  // Unix ms

    // assistant fields
    std::string api;
    std::string provider;
    std::string model;
    std::string responseModel;
    std::string responseId;
    std::string errorMessage;
    Usage usage;
    StopReason stopReason = StopReason::Stop;

    // toolResult fields
    std::string toolCallId;
    std::string toolName;
    bool isError = false;
    Json details = Json::object();

    static Message user(std::string text, int64_t timestamp = 0)
    {
        Message m;
        m.role = Role::User;
        m.timestamp = timestamp;
        m.content.push_back(ContentBlock{});
        m.content.back().type = BlockType::Text;
        m.content.back().text = std::move(text);
        return m;
    }

    static Message toolResult(std::string callId, std::string name, std::string text, bool isError,
                              int64_t timestamp = 0)
    {
        Message m;
        m.role = Role::ToolResult;
        m.timestamp = timestamp;
        m.toolCallId = std::move(callId);
        m.toolName = std::move(name);
        m.isError = isError;
        m.content.push_back(ContentBlock{});
        m.content.back().type = BlockType::Text;
        m.content.back().text = std::move(text);
        return m;
    }

    /** Combined text of all text blocks. */
    std::string text_content() const
    {
        std::string out;
        for (const auto& block : content)
        {
            if (block.type == BlockType::Text)
            {
                out += block.text;
            }
        }
        return out;
    }

    bool has_tool_calls() const
    {
        for (const auto& block : content)
        {
            if (block.type == BlockType::ToolCall) return true;
        }
        return false;
    }
};

/** Tool definition (JSON Schema parameters). */
struct Tool
{
    std::string name;
    std::string description;
    Json parameters = Json::object();
};

}  // namespace pi

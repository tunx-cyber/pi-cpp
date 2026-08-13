#include "pi/ai/openai_transport.h"

#include <curl/curl.h>

#include <cctype>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <openai/client.hpp>
#include <openai/error.hpp>
#include <optional>
#include <set>

#include "pi/ai/cost.h"
#include "pi/ai/json_util.h"

namespace pi
{

namespace
{

using openai_json = nlohmann::json;

/** 兼容性设置，镜像 pi 的 OpenAICompletionsCompat（detectCompat 的 deepseek 分支）。 */
struct Compat
{
    bool supportsStore = false;
    bool supportsDeveloperRole = false;
    bool supportsReasoningEffort = true;
    bool supportsUsageInStreaming = true;
    std::string maxTokensField = "max_completion_tokens";
    bool requiresToolResultName = false;
    bool requiresAssistantAfterToolResult = false;
    bool requiresThinkingAsText = false;
    bool requiresReasoningContentOnAssistantMessages = false;
    std::string thinkingFormat = "openai";
    bool supportsStrictMode = true;
};

Compat detect_compat(const ModelInfo& model)
{
    const auto& baseUrl = model.baseUrl;
    const bool isDeepSeek =
        model.provider == "deepseek" || baseUrl.find("deepseek.com") != std::string::npos;
    const bool isZai = model.provider == "zai" || baseUrl.find("api.z.ai") != std::string::npos;
    const bool isMoonshot =
        model.provider == "moonshotai" || baseUrl.find("api.moonshot.") != std::string::npos;
    const bool isTogether = baseUrl.find("api.together.ai") != std::string::npos ||
                            baseUrl.find("api.together.xyz") != std::string::npos;
    const bool isGrok = model.provider == "xai" || baseUrl.find("api.x.ai") != std::string::npos;

    const bool isNonStandard =
        model.provider == "cerebras" || baseUrl.find("cerebras.ai") != std::string::npos ||
        isTogether || baseUrl.find("chutes.ai") != std::string::npos ||
        baseUrl.find("deepseek.com") != std::string::npos || isZai || isMoonshot;

    // 维护提示：这些字段镜像 pi 的 OpenAICompletionsCompat，供未来 provider 分支启用。
    // 目前所有 provider 都未启用 requiresToolResultName / requiresAssistantAfterToolResult /
    // requiresThinkingAsText / supportsStore；新增 provider 时对照 pi 的 detectCompat 设置。
    Compat compat;
    compat.supportsStore = !isNonStandard;
    compat.supportsDeveloperRole = !isNonStandard;
    compat.supportsReasoningEffort = !isGrok && !isZai && !isMoonshot && !isTogether;
    compat.maxTokensField = isMoonshot || isTogether ? "max_tokens" : "max_completion_tokens";
    compat.requiresReasoningContentOnAssistantMessages = isDeepSeek;
    compat.thinkingFormat = isDeepSeek   ? "deepseek"
                            : isZai      ? "zai"
                            : isTogether ? "together"
                                         : "openai";
    compat.supportsStrictMode = !isMoonshot && !isTogether;
    return compat;
}

std::string normalize_tool_call_id(const std::string& id, const ModelInfo& model)
{
    if (id.find('|') != std::string::npos)
    {
        // 镜像 pi：带 | 的 id 取前段并替换非法字符（部分 provider 的 id 由模型名|调用号组成）。
        const std::string call_id = id.substr(0, id.find('|'));
        std::string sanitized;
        for (char c : call_id)
        {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-';
            sanitized += ok ? c : '_';
        }
        // 截断到 40 字符（OpenAI tool call id 上限）；若净化后为空则保留原 id。
        if (sanitized.size() > 40) sanitized.resize(40);
        return sanitized.empty() ? id : sanitized;
    }
    if (model.provider == "openai" && id.size() > 40) return id.substr(0, 40);
    return id;
}

constexpr const char* kNonVisionUserImagePlaceholder =
    "(image omitted: model does not support images)";
constexpr const char* kNonVisionToolImagePlaceholder =
    "(tool image omitted: model does not support images)";

std::vector<ContentBlock> replace_images_with_placeholder(const std::vector<ContentBlock>& content,
                                                          const std::string& placeholder)
{
    std::vector<ContentBlock> result;
    bool previous_was_placeholder = false;
    for (const auto& block : content)
    {
        if (block.type == BlockType::Image)
        {
            if (!previous_was_placeholder)
            {
                ContentBlock text;
                text.type = BlockType::Text;
                text.text = placeholder;
                result.push_back(std::move(text));
            }
            previous_was_placeholder = true;
            continue;
        }
        result.push_back(block);
        previous_was_placeholder = (block.type == BlockType::Text && block.text == placeholder);
    }
    return result;
}

/**
 * 消息转换（镜像 pi 的 transformMessages）：
 * 非视觉模型图片降级、thinking 块跨模型处理、tool call id 归一化、
 * 跳过 error/aborted 的 assistant 消息、为孤儿 tool call 补合成 toolResult。
 */
std::vector<Message> transform_messages(const std::vector<Message>& messages,
                                        const ModelInfo& model)
{
    std::map<std::string, std::string> tool_call_id_map;

    std::vector<Message> transformed;
    for (const auto& msg : messages)
    {
        Message out = msg;
        if (msg.role == Role::User && !model.supports_images())
        {
            out.content =
                replace_images_with_placeholder(msg.content, kNonVisionUserImagePlaceholder);
        }
        else if (msg.role == Role::ToolResult && !model.supports_images())
        {
            out.content =
                replace_images_with_placeholder(msg.content, kNonVisionToolImagePlaceholder);
        }
        else if (msg.role == Role::Assistant)
        {
            const bool same_model =
                msg.provider == model.provider && msg.api == model.api && msg.model == model.id;
            std::vector<ContentBlock> content;
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Thinking)
                {
                    if (block.redacted)
                    {
                        if (same_model) content.push_back(block);
                        continue;
                    }
                    if (same_model && !block.thinkingSignature.empty())
                    {
                        content.push_back(block);
                        continue;
                    }
                    std::string trimmed;
                    for (char c : block.thinking)
                    {
                        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') trimmed += c;
                    }
                    if (trimmed.empty()) continue;
                    if (same_model)
                    {
                        content.push_back(block);
                    }
                    else
                    {
                        ContentBlock text;
                        text.type = BlockType::Text;
                        text.text = block.thinking;
                        content.push_back(std::move(text));
                    }
                }
                else if (block.type == BlockType::Text)
                {
                    content.push_back(block);
                }
                else if (block.type == BlockType::ToolCall)
                {
                    ContentBlock call = block;
                    if (!same_model && !call.thoughtSignature.empty())
                        call.thoughtSignature.clear();
                    if (!same_model)
                    {
                        const std::string normalized = normalize_tool_call_id(call.id, model);
                        if (normalized != call.id)
                        {
                            tool_call_id_map[call.id] = normalized;
                            call.id = normalized;
                        }
                    }
                    content.push_back(std::move(call));
                }
                else
                {
                    content.push_back(block);
                }
            }
            out.content = std::move(content);
        }
        transformed.push_back(std::move(out));
    }

    // 第二遍：孤儿 tool call 补合成 toolResult
    std::vector<Message> result;
    std::vector<const ContentBlock*> pending_tool_calls;
    std::set<std::string> existing_tool_result_ids;

    auto insert_synthetic = [&]()
    {
        if (pending_tool_calls.empty()) return;
        for (const auto* tc : pending_tool_calls)
        {
            if (existing_tool_result_ids.count(tc->id) == 0)
            {
                result.push_back(Message::toolResult(tc->id, tc->name, "No result provided", true));
            }
        }
        pending_tool_calls.clear();
        existing_tool_result_ids.clear();
    };

    for (const auto& msg : transformed)
    {
        if (msg.role == Role::Assistant)
        {
            insert_synthetic();
            if (msg.stopReason == StopReason::Error || msg.stopReason == StopReason::Aborted)
                continue;
            pending_tool_calls.clear();
            existing_tool_result_ids.clear();
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::ToolCall) pending_tool_calls.push_back(&block);
            }
            result.push_back(msg);
        }
        else if (msg.role == Role::ToolResult)
        {
            existing_tool_result_ids.insert(msg.toolCallId);
            result.push_back(msg);
        }
        else if (msg.role == Role::User)
        {
            insert_synthetic();
            result.push_back(msg);
        }
        else
        {
            result.push_back(msg);
        }
    }
    insert_synthetic();
    return result;
}

bool has_tool_history(const std::vector<Message>& messages)
{
    for (const auto& msg : messages)
    {
        if (msg.role == Role::ToolResult) return true;
        if (msg.role == Role::Assistant && msg.has_tool_calls()) return true;
    }
    return false;
}

/**
 * 消息 → wire 格式（镜像 pi 的 convertMessages）。
 * 返回 openai-cpp 的 ChatMessage 列表，以及每条 assistant 消息需要注入的额外字段
 * （deepseek 的 reasoning_content），由 body augmenter 写回。
 */
struct ConvertedMessages
{
    std::vector<openai::ChatMessage> messages;
    // 不变量：assistantExtras[i] 与 messages 中第 i 条 assistant 消息一一对应（包括
    // requiresAssistantAfterToolResult 插入的桥接消息，其 extras 为空对象）。
    // body augmenter 依赖此顺序回注 deepseek 的 reasoning_content 等字段。
    std::vector<openai_json> assistantExtras;
};

ConvertedMessages convert_messages(const ModelInfo& model, const std::vector<Message>& messages,
                                   const std::string& systemPrompt, const Compat& compat)
{
    ConvertedMessages out;
    const bool use_developer_role = model.reasoning && compat.supportsDeveloperRole;

    if (!systemPrompt.empty())
    {
        openai::ChatMessage sys;
        sys.role = use_developer_role ? "developer" : "system";
        openai::ChatMessageContent part;
        part.type = openai::ChatMessageContent::Type::Text;
        part.text = systemPrompt;
        sys.content.push_back(std::move(part));
        out.messages.push_back(std::move(sys));
    }

    std::string last_role;
    std::vector<openai_json> pending_extras;

    for (size_t i = 0; i < messages.size(); ++i)
    {
        const auto& msg = messages[i];

        if (msg.role == Role::User)
        {
            openai::ChatMessage wire;
            wire.role = "user";
            bool any = false;
            if (msg.content.size() == 1 && msg.content[0].type == BlockType::Text)
            {
                wire.content.push_back({});
                wire.content.back().type = openai::ChatMessageContent::Type::Text;
                wire.content.back().text = msg.content[0].text;
                any = true;
            }
            else
            {
                for (const auto& block : msg.content)
                {
                    openai::ChatMessageContent part;
                    if (block.type == BlockType::Text)
                    {
                        part.type = openai::ChatMessageContent::Type::Text;
                        part.text = block.text;
                    }
                    else if (block.type == BlockType::Image)
                    {
                        part.type = openai::ChatMessageContent::Type::Image;
                        part.image_url = "data:" + block.mimeType + ";base64," + block.data;
                    }
                    else
                    {
                        continue;
                    }
                    wire.content.push_back(std::move(part));
                    any = true;
                }
            }
            if (!any) continue;
            out.messages.push_back(std::move(wire));
        }
        else if (msg.role == Role::Assistant)
        {
            openai::ChatMessage wire;
            wire.role = "assistant";

            std::string assistant_text;
            bool has_thoughts = false;
            std::string signature;
            std::string joined_thinking;
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Text) assistant_text += block.text;
            }
            for (const auto& block : msg.content)
            {
                if (block.type != BlockType::Thinking) continue;
                std::string trimmed;
                for (char c : block.thinking)
                {
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') trimmed += c;
                }
                if (trimmed.empty()) continue;
                has_thoughts = true;
                if (signature.empty() && !block.thinkingSignature.empty())
                    signature = block.thinkingSignature;
                if (!joined_thinking.empty()) joined_thinking += "\n";
                joined_thinking += block.thinking;
            }

            openai_json extra = openai_json::object();
            if (has_thoughts)
            {
                if (compat.requiresThinkingAsText)
                {
                    openai::ChatMessageContent part;
                    part.type = openai::ChatMessageContent::Type::Text;
                    part.text = joined_thinking;
                    wire.content.push_back(std::move(part));
                    if (!assistant_text.empty())
                    {
                        openai::ChatMessageContent text_part;
                        text_part.type = openai::ChatMessageContent::Type::Text;
                        text_part.text = assistant_text;
                        wire.content.push_back(std::move(text_part));
                    }
                }
                else
                {
                    if (!assistant_text.empty())
                    {
                        openai::ChatMessageContent text_part;
                        text_part.type = openai::ChatMessageContent::Type::Text;
                        text_part.text = assistant_text;
                        wire.content.push_back(std::move(text_part));
                    }
                    // 用第一个 thinking 块的 signature 作为 reasoning 字段名
                    if (!signature.empty()) extra[signature] = joined_thinking;
                }
            }
            else if (!assistant_text.empty())
            {
                openai::ChatMessageContent text_part;
                text_part.type = openai::ChatMessageContent::Type::Text;
                text_part.text = assistant_text;
                wire.content.push_back(std::move(text_part));
            }

            std::vector<openai::ChatCompletionToolCall> tool_calls;
            for (const auto& block : msg.content)
            {
                if (block.type != BlockType::ToolCall) continue;
                openai::ChatCompletionToolCall call;
                call.id = block.id;
                call.type = "function";
                openai_json fn = openai_json::object();
                fn["name"] = block.name;
                fn["arguments"] = block.arguments.dump();
                call.function = std::move(fn);
                tool_calls.push_back(std::move(call));
            }
            wire.tool_calls = std::move(tool_calls);

            if (compat.requiresReasoningContentOnAssistantMessages && model.reasoning &&
                !extra.contains("reasoning_content"))
            {
                extra["reasoning_content"] = "";
            }

            const bool has_content = !wire.content.empty() || !assistant_text.empty();
            if (!has_content && wire.tool_calls.empty())
            {
                continue;  // 跳过既无内容又无工具调用的 assistant 消息
            }
            pending_extras.push_back(std::move(extra));
            out.messages.push_back(std::move(wire));
        }
        else if (msg.role == Role::ToolResult)
        {
            // 连续的 toolResult 合并（镜像 TS 的游标处理）
            std::vector<ContentBlock> image_blocks;
            size_t j = i;
            for (; j < messages.size() && messages[j].role == Role::ToolResult; ++j)
            {
                const auto& toolMsg = messages[j];
                std::string text_result;
                for (const auto& block : toolMsg.content)
                {
                    if (block.type == BlockType::Text)
                    {
                        if (!text_result.empty()) text_result += "\n";
                        text_result += block.text;
                    }
                }
                const bool has_images = [&]
                {
                    for (const auto& block : toolMsg.content)
                    {
                        if (block.type == BlockType::Image) return true;
                    }
                    return false;
                }();
                if (has_images)
                {
                    for (const auto& block : toolMsg.content)
                    {
                        if (block.type == BlockType::Image) image_blocks.push_back(block);
                    }
                }

                openai::ChatMessage tool;
                tool.role = "tool";
                openai::ChatMessageContent part;
                part.type = openai::ChatMessageContent::Type::Text;
                part.text = text_result.empty() ? "(see attached image)" : text_result;
                tool.content.push_back(std::move(part));
                tool.tool_call_id = toolMsg.toolCallId;
                if (compat.requiresToolResultName && !toolMsg.toolName.empty())
                    tool.name = toolMsg.toolName;
                out.messages.push_back(std::move(tool));
            }
            i = j - 1;

            if (!image_blocks.empty() && model.supports_images())
            {
                if (compat.requiresAssistantAfterToolResult)
                {
                    openai::ChatMessage bridge;
                    bridge.role = "assistant";
                    openai::ChatMessageContent part;
                    part.type = openai::ChatMessageContent::Type::Text;
                    part.text = "I have processed the tool results.";
                    bridge.content.push_back(std::move(part));
                    out.messages.push_back(std::move(bridge));
                    // 桥接消息同样占据一个 assistant 槽位，推送空 extras 保持序号对齐
                    pending_extras.push_back(openai_json::object());
                }
                openai::ChatMessage user;
                user.role = "user";
                openai::ChatMessageContent label;
                label.type = openai::ChatMessageContent::Type::Text;
                label.text = "Attached image(s) from tool result:";
                user.content.push_back(std::move(label));
                for (const auto& block : image_blocks)
                {
                    openai::ChatMessageContent img;
                    img.type = openai::ChatMessageContent::Type::Image;
                    img.image_url = "data:" + block.mimeType + ";base64," + block.data;
                    user.content.push_back(std::move(img));
                }
                out.messages.push_back(std::move(user));
                last_role = "user";
            }
            else
            {
                last_role = "toolResult";
            }
            continue;
        }
        last_role = msg.role == Role::ToolResult ? "toolResult"
                                                 : (msg.role == Role::User ? "user" : "assistant");
    }

    out.assistantExtras = std::move(pending_extras);
    return out;
}

}  // namespace

class OpenAiCompletionsTransport::Impl
{
   public:
    Impl(std::string baseUrl, std::string apiKey, std::chrono::milliseconds timeout)
        : baseUrl_(std::move(baseUrl)),
          apiKey_(std::move(apiKey)),
          timeout_(timeout),
          abort_(std::make_shared<std::atomic<bool>>(false))
    {
    }

    void stream_chat(const ModelInfo& model, const std::vector<Message>& messages,
                     const StreamRequestOptions& opts,
                     const std::function<void(const StreamEvent&)>& sink)
    {
        // The underlying client and body augmenter are mutable per request.
        std::lock_guard<std::mutex> lock(stream_mutex_);
        abort_ = opts.abort ? opts.abort : std::make_shared<std::atomic<bool>>(false);

        auto output = make_output(model);
        sink(StreamEvent{StreamEvent::Type::Start, -1, "", "", {}, StopReason::Stop, output});

        const std::string request_api_key =
            opts.apiKey && !opts.apiKey->empty() ? *opts.apiKey : apiKey_;
        if (request_api_key.empty())
        {
            output.stopReason = StopReason::Error;
            output.errorMessage =
                "Missing API key. Set PI_API_KEY, DEEPSEEK_API_KEY, or OPENAI_API_KEY before sending a prompt.";
            StreamEvent error;
            error.type = StreamEvent::Type::Error;
            error.reason = StopReason::Error;
            error.message = output;
            sink(error);
            return;
        }
        try
        {
            client_ = make_client(request_api_key);
        }
        catch (const std::exception& e)
        {
            output.stopReason = StopReason::Error;
            output.errorMessage = e.what();
            StreamEvent error;
            error.type = StreamEvent::Type::Error;
            error.reason = StopReason::Error;
            error.message = output;
            sink(error);
            return;
        }

        const Compat compat = detect_compat(model);
        const std::string systemPrompt = opts.systemPrompt;
        const auto transformed = transform_messages(messages, model);
        const auto converted = convert_messages(model, transformed, systemPrompt, compat);

        // body augmenter 配置：top-level thinking + assistant 消息额外字段
        Json augment_config = Json::object();
        if (model.reasoning && compat.thinkingFormat == "deepseek")
        {
            augment_config["thinking"] = Json{
                {"type",
                 opts.reasoning && *opts.reasoning != ThinkingLevel::Off ? "enabled" : "disabled"}};
        }
        Json extras = Json::array();
        for (const auto& extra : converted.assistantExtras) extras.push_back(extra);
        set_augmenter(augment_config, std::move(extras));

        openai::ChatCompletionRequest request;
        request.model = model.id;
        request.messages = converted.messages;
        if (opts.maxTokens)
        {
            if (compat.maxTokensField == "max_tokens")
            {
                request.max_tokens = *opts.maxTokens;
            }
            else
            {
                request.max_completion_tokens = *opts.maxTokens;
            }
        }
        if (opts.temperature) request.temperature = *opts.temperature;
        if (compat.supportsUsageInStreaming)
        {
            openai::ChatCompletionStreamOptions stream_options;
            stream_options.include_usage = true;
            request.stream_options = stream_options;
        }
        if (opts.reasoning && model.reasoning && compat.supportsReasoningEffort)
        {
            const auto level = *opts.reasoning;
            if (level != ThinkingLevel::Off)
            {
                const auto it = model.thinkingLevelMap.find(level);
                request.reasoning_effort = it != model.thinkingLevelMap.end() && it->second
                                               ? *it->second
                                               : to_string(level);
            }
            else
            {
                const auto offIt = model.thinkingLevelMap.find(ThinkingLevel::Off);
                if (offIt != model.thinkingLevelMap.end() && offIt->second)
                    request.reasoning_effort = *offIt->second;
            }
        }

        // 工具（M3 才启用，先把转换写好）
        if (!opts.tools.empty())
        {
            request.tools = convert_tools(opts.tools, compat);
        }
        else if (has_tool_history(messages))
        {
            request.tools = {};
        }

        openai::RequestOptions request_options;
        request_options.max_retries = static_cast<size_t>(opts.maxRetries);
        request_options.timeout = opts.timeoutMs;
        // 不手动注入 Authorization：客户端已用 request_api_key + use_bearer_auth 构造，
        // openai-cpp 会自行添加 "Bearer <key>"。这里重复注入会依赖服务端对重复头的处理。
        for (const auto& [key, value] : opts.headers)
        {
            request_options.headers[key] = value;
        }
        if (opts.onResponse)
        {
            // openai-cpp 不暴露原始响应头回调；状态码在异常中携带，此处留空
        }

        StreamContext ctx{output, sink, model};

        try
        {
            client_->chat().completions().stream(
                request, [this, &ctx](const openai::ServerSentEvent& sse) -> bool
                { return handle_sse_event(ctx, sse); }, request_options);

            for (size_t i = 0; i < ctx.output.content.size(); ++i)
            {
                finish_block(ctx, static_cast<int>(i));
            }
            if (abort_->load())
            {
                throw std::runtime_error("Request was aborted");
            }
            if (ctx.output.stopReason == StopReason::Aborted)
            {
                throw std::runtime_error("Request was aborted");
            }
            if (ctx.output.stopReason == StopReason::Error)
            {
                throw std::runtime_error(ctx.output.errorMessage.empty()
                                             ? "Provider returned an error stop reason"
                                             : ctx.output.errorMessage);
            }
            if (!ctx.hasFinishReason)
            {
                throw std::runtime_error(
                    "Stream ended without finish_reason" +
                    (ctx.malformedChunks > 0
                         ? " (" + std::to_string(ctx.malformedChunks) + " malformed chunk(s))"
                         : ""));
            }
            StreamEvent done;
            done.type = StreamEvent::Type::Done;
            done.reason = ctx.output.stopReason;
            done.message = ctx.output;
            sink(done);
        }
        catch (const std::exception& e)
        {
            ctx.output.stopReason = abort_->load() ? StopReason::Aborted : StopReason::Error;
            ctx.output.errorMessage = e.what();
            StreamEvent err;
            err.type = StreamEvent::Type::Error;
            err.reason = ctx.output.stopReason;
            err.message = ctx.output;
            sink(err);
        }
    }

   private:
    std::unique_ptr<openai::OpenAIClient> make_client(const std::string& apiKey)
    {
        openai::ClientOptions options;
        options.api_key = apiKey;
        options.base_url = baseUrl_;
        options.timeout = timeout_;
        options.max_retries = 0;
        options.use_bearer_auth = true;
        return std::make_unique<openai::OpenAIClient>(options, make_http_client());
    }

    struct StreamContext
    {
        Message& output;
        const std::function<void(const StreamEvent&)>& sink;
        const ModelInfo& model;
        bool hasFinishReason = false;
        int malformedChunks = 0;  // 无法解析的 SSE chunk 数（计入最终错误便于排查）
        int textBlockIndex = -1;
        int thinkingBlockIndex = -1;
        std::vector<std::string> partialArgs;
        std::vector<int> streamIndexes;
        std::map<int, int> byStreamIndex;
        std::map<std::string, int> byCallId;
    };

    Message make_output(const ModelInfo& model)
    {
        Message out;
        out.role = Role::Assistant;
        out.api = model.api;
        out.provider = model.provider;
        out.model = model.id;
        out.usage = Usage{};
        out.stopReason = StopReason::Stop;
        out.timestamp = 0;
        return out;
    }

    static void emit_block_start(StreamContext& ctx, StreamEvent::Type type, int index)
    {
        StreamEvent event;
        event.type = type;
        event.contentIndex = index;
        event.message = ctx.output;
        ctx.sink(event);
    }

    bool handle_sse_event(StreamContext& ctx, const openai::ServerSentEvent& sse)
    {
        if (abort_->load()) return false;

        Json chunk;
        try
        {
            chunk = Json::parse(sse.data);
        }
        catch (...)
        {
            ++ctx.malformedChunks;
            return !abort_->load();
        }
        if (!chunk.is_object())
        {
            ++ctx.malformedChunks;
            return !abort_->load();
        }

        auto& output = ctx.output;

        // responseId / responseModel
        if (output.responseId.empty() && chunk.contains("id") && chunk["id"].is_string())
        {
            output.responseId = chunk["id"].get<std::string>();
        }
        if (chunk.contains("model") && chunk["model"].is_string())
        {
            const std::string model_id = chunk["model"].get<std::string>();
            if (!model_id.empty() && model_id != ctx.model.id && output.responseModel.empty())
            {
                output.responseModel = model_id;
            }
        }

        if (chunk.contains("usage") && chunk["usage"].is_object())
        {
            output.usage = parse_chunk_usage(chunk["usage"], ctx.model);
        }

        Json choice;
        bool has_choice = false;
        if (chunk.contains("choices") && chunk["choices"].is_array() && !chunk["choices"].empty())
        {
            choice = chunk["choices"][0];
            has_choice = choice.is_object();
        }
        if (has_choice)
        {
            if (!chunk.contains("usage") && choice.contains("usage") && choice["usage"].is_object())
            {
                output.usage = parse_chunk_usage(choice["usage"], ctx.model);
            }

            if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
            {
                const auto mapping = map_stop_reason(choice["finish_reason"].get<std::string>());
                output.stopReason = mapping.reason;
                if (!mapping.errorMessage.empty()) output.errorMessage = mapping.errorMessage;
                ctx.hasFinishReason = true;
            }

            if (choice.contains("delta") && choice["delta"].is_object())
            {
                const Json& delta = choice["delta"];

                if (delta.contains("content") && delta["content"].is_string() &&
                    !delta["content"].get<std::string>().empty())
                {
                    const std::string text = delta["content"].get<std::string>();
                    ensure_text_block(ctx);
                    output.content[ctx.textBlockIndex].text += text;
                    StreamEvent event;
                    event.type = StreamEvent::Type::TextDelta;
                    event.contentIndex = ctx.textBlockIndex;
                    event.delta = text;
                    event.message = output;
                    ctx.sink(event);
                }

                // reasoning_content / reasoning / reasoning_text
                static const char* kReasoningFields[] = {"reasoning_content", "reasoning",
                                                         "reasoning_text"};
                std::string found_reasoning;
                for (const char* field : kReasoningFields)
                {
                    if (delta.contains(field) && delta[field].is_string() &&
                        !delta[field].get<std::string>().empty())
                    {
                        found_reasoning = field;
                        break;
                    }
                }
                if (!found_reasoning.empty())
                {
                    const std::string text = delta[found_reasoning].get<std::string>();
                    ensure_thinking_block(ctx, found_reasoning);
                    output.content[ctx.thinkingBlockIndex].thinking += text;
                    StreamEvent event;
                    event.type = StreamEvent::Type::ThinkingDelta;
                    event.contentIndex = ctx.thinkingBlockIndex;
                    event.delta = text;
                    event.message = output;
                    ctx.sink(event);
                }

                if (delta.contains("tool_calls") && delta["tool_calls"].is_array())
                {
                    for (const auto& toolCall : delta["tool_calls"])
                    {
                        if (!toolCall.is_object()) continue;
                        const int block_index = ensure_tool_call_block(ctx, toolCall);
                        std::string delta_text;
                        if (toolCall.contains("function") && toolCall["function"].is_object() &&
                            toolCall["function"].contains("arguments") &&
                            toolCall["function"]["arguments"].is_string())
                        {
                            delta_text = toolCall["function"]["arguments"].get<std::string>();
                            ctx.partialArgs[block_index] += delta_text;
                            output.content[block_index].arguments =
                                parse_streaming_json(ctx.partialArgs[block_index]);
                        }
                        StreamEvent event;
                        event.type = StreamEvent::Type::ToolCallDelta;
                        event.contentIndex = block_index;
                        event.delta = delta_text;
                        event.message = output;
                        ctx.sink(event);
                    }
                }

                // reasoning_details → thoughtSignature
                if (delta.contains("reasoning_details") && delta["reasoning_details"].is_array())
                {
                    for (const auto& detail : delta["reasoning_details"])
                    {
                        if (!detail.is_object()) continue;
                        if (detail.value("type", "") == "reasoning.encrypted" &&
                            detail.contains("id") && detail.contains("data"))
                        {
                            const std::string id = detail["id"].get<std::string>();
                            for (auto& block : output.content)
                            {
                                if (block.type == BlockType::ToolCall && block.id == id)
                                {
                                    block.thoughtSignature = detail.dump();
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        return !abort_->load();
    }

    void ensure_text_block(StreamContext& ctx)
    {
        auto& output = ctx.output;
        if (ctx.textBlockIndex >= 0) return;
        ContentBlock block;
        block.type = BlockType::Text;
        output.content.push_back(std::move(block));
        ctx.textBlockIndex = static_cast<int>(output.content.size()) - 1;
        ctx.partialArgs.emplace_back();
        ctx.streamIndexes.push_back(-1);
        emit_block_start(ctx, StreamEvent::Type::TextStart, ctx.textBlockIndex);
    }

    void ensure_thinking_block(StreamContext& ctx, const std::string& signature)
    {
        auto& output = ctx.output;
        if (ctx.thinkingBlockIndex >= 0) return;
        ContentBlock block;
        block.type = BlockType::Thinking;
        block.thinkingSignature = signature;
        output.content.push_back(std::move(block));
        ctx.thinkingBlockIndex = static_cast<int>(output.content.size()) - 1;
        ctx.partialArgs.emplace_back();
        ctx.streamIndexes.push_back(-1);
        emit_block_start(ctx, StreamEvent::Type::ThinkingStart, ctx.thinkingBlockIndex);
    }

    int ensure_tool_call_block(StreamContext& ctx, const Json& toolCall)
    {
        auto& output = ctx.output;
        const int stream_index = toolCall.contains("index") && toolCall["index"].is_number()
                                     ? toolCall["index"].get<int>()
                                     : -1;
        std::string id;
        if (toolCall.contains("id") && toolCall["id"].is_string())
            id = toolCall["id"].get<std::string>();

        int block_index = -1;
        if (stream_index >= 0)
        {
            const auto it = ctx.byStreamIndex.find(stream_index);
            if (it != ctx.byStreamIndex.end()) block_index = it->second;
        }
        if (block_index < 0 && !id.empty())
        {
            const auto it = ctx.byCallId.find(id);
            if (it != ctx.byCallId.end()) block_index = it->second;
        }
        if (block_index < 0)
        {
            ContentBlock block;
            block.type = BlockType::ToolCall;
            block.id = id;
            if (toolCall.contains("function") && toolCall["function"].is_object() &&
                toolCall["function"].contains("name") && toolCall["function"]["name"].is_string())
            {
                block.name = toolCall["function"]["name"].get<std::string>();
            }
            output.content.push_back(std::move(block));
            block_index = static_cast<int>(output.content.size()) - 1;
            ctx.partialArgs.emplace_back();
            ctx.streamIndexes.push_back(stream_index);
            if (stream_index >= 0) ctx.byStreamIndex[stream_index] = block_index;
            if (!id.empty()) ctx.byCallId[id] = block_index;
            emit_block_start(ctx, StreamEvent::Type::ToolCallStart, block_index);
        }
        if (stream_index >= 0 && ctx.streamIndexes[block_index] < 0)
        {
            ctx.streamIndexes[block_index] = stream_index;
            ctx.byStreamIndex[stream_index] = block_index;
        }
        if (!id.empty()) ctx.byCallId[id] = block_index;

        auto& block = output.content[block_index];
        if (block.id.empty() && !id.empty()) block.id = id;
        if (block.name.empty() && toolCall.contains("function") &&
            toolCall["function"].is_object() && toolCall["function"].contains("name") &&
            toolCall["function"]["name"].is_string())
        {
            block.name = toolCall["function"]["name"].get<std::string>();
        }
        return block_index;
    }

    void finish_block(StreamContext& ctx, int index)
    {
        auto& block = ctx.output.content[index];
        StreamEvent event;
        event.contentIndex = index;
        event.message = ctx.output;
        switch (block.type)
        {
            case BlockType::Text:
                event.type = StreamEvent::Type::TextEnd;
                event.content = block.text;
                break;
            case BlockType::Thinking:
                event.type = StreamEvent::Type::ThinkingEnd;
                event.content = block.thinking;
                break;
            case BlockType::ToolCall:
                block.arguments = parse_streaming_json(ctx.partialArgs[index]);
                event.type = StreamEvent::Type::ToolCallEnd;
                event.toolCall = block;
                break;
            default:
                return;
        }
        ctx.sink(event);
    }

    static Usage parse_chunk_usage(const Json& raw, const ModelInfo& model)
    {
        Usage usage;
        int64_t prompt = raw.value("prompt_tokens", 0);
        int64_t completion = raw.value("completion_tokens", 0);
        int64_t cache_read = 0;
        int64_t cache_write = 0;
        if (raw.contains("prompt_tokens_details") && raw["prompt_tokens_details"].is_object())
        {
            const auto& d = raw["prompt_tokens_details"];
            cache_read = d.value("cached_tokens", 0);
            cache_write = d.value("cache_write_tokens", 0);
        }
        else
        {
            cache_read = raw.value("prompt_cache_hit_tokens", 0);
        }
        usage.input = std::max<int64_t>(0, prompt - cache_read - cache_write);
        usage.output = completion;
        usage.cacheRead = cache_read;
        usage.cacheWrite = cache_write;
        usage.totalTokens = usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
        usage.cost = calculate_cost(model, usage);
        return usage;
    }

    struct StopMapping
    {
        StopReason reason;
        std::string errorMessage;
    };

    static StopMapping map_stop_reason(const std::string& reason)
    {
        if (reason == "stop" || reason == "end") return {StopReason::Stop, ""};
        if (reason == "length") return {StopReason::Length, ""};
        if (reason == "function_call" || reason == "tool_calls") return {StopReason::ToolUse, ""};
        if (reason == "content_filter")
            return {StopReason::Error, "Provider finish_reason: content_filter"};
        if (reason == "network_error")
            return {StopReason::Error, "Provider finish_reason: network_error"};
        return {StopReason::Error, "Provider finish_reason: " + reason};
    }

    static std::vector<openai::ChatCompletionToolDefinition> convert_tools(
        const std::vector<Tool>& tools, const Compat& compat)
    {
        std::vector<openai::ChatCompletionToolDefinition> out;
        for (const auto& tool : tools)
        {
            openai::ChatCompletionToolDefinition def;
            def.type = "function";
            openai::ChatToolFunctionDefinition fn;
            fn.name = tool.name;
            fn.description = tool.description;
            fn.parameters = tool.parameters;
            def.function = fn;
            if (compat.supportsStrictMode)
            {
                def.raw["strict"] = false;
            }
            out.push_back(std::move(def));
        }
        return out;
    }

    // ---- body augmenter：注入 openai-cpp 无法表达的开创字段 ----

    void set_augmenter(const Json& topLevel, Json assistantExtras)
    {
        augment_config_ = topLevel;
        assistant_extras_ = std::move(assistantExtras);
    }

    std::unique_ptr<openai::HttpClient> make_http_client();

    std::string baseUrl_;
    std::string apiKey_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<std::atomic<bool>> abort_;
    std::unique_ptr<openai::OpenAIClient> client_;
    Json augment_config_ = Json::object();
    Json assistant_extras_ = Json::array();
    std::mutex stream_mutex_;
};

// 自定义 HttpClient：abort 感知 + body augmenter（在 src 内实现 curl 部分）
class AbortableHttpClient : public openai::HttpClient
{
   public:
    AbortableHttpClient(std::shared_ptr<std::atomic<bool>> abort,
                        std::function<void(Json&)> augmenter)
        : abort_(std::move(abort)), augmenter_(std::move(augmenter))
    {
        static bool curl_initialized = []
        {
            curl_global_init(CURL_GLOBAL_DEFAULT);
            return true;
        }();
        (void)curl_initialized;
    }

    openai::HttpResponse request(const openai::HttpRequest& req) override
    {
        openai::HttpRequest request = req;
        if (augmenter_ && !request.body.empty())
        {
            try
            {
                Json body = Json::parse(request.body);
                augmenter_(body);
                request.body = body.dump();
            }
            catch (...)
            {
            }
        }
        return do_curl(request);
    }

   private:
    struct RequestState
    {
        std::string* body;
        std::function<void(const char*, std::size_t)>* on_chunk;
        AbortableHttpClient* self;
        bool error = false;
        long status = 0;
        std::string error_body;  // 流式模式下仍收集错误响应体，用于错误诊断
        std::map<std::string, std::string> headers;
    };

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* ctx = static_cast<RequestState*>(userdata);
        const size_t total = size * nmemb;
        if (ctx->self->abort_->load())
        {
            ctx->error = true;
            return 0;  // 中止传输（CURLE_WRITE_ERROR）
        }
        if (ctx->status >= 400)
        {
            ctx->error_body.append(ptr, total);
            return total;
        }
        if (ctx->on_chunk && *ctx->on_chunk)
        {
            (*ctx->on_chunk)(ptr, total);
        }
        if (ctx->body)
        {
            ctx->body->append(ptr, total);
        }
        return total;
    }

    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata)
    {
        const size_t total = size * nitems;
        std::string line(buffer, total);
        auto* state = static_cast<RequestState*>(userdata);
        if (line.rfind("HTTP/", 0) == 0)
        {
            // "HTTP/1.1 400 Bad Request" → 提取状态码
            size_t space = line.find(' ');
            if (space != std::string::npos)
            {
                state->status = std::strtol(line.c_str() + space + 1, nullptr, 10);
            }
            return total;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            auto trim = [](std::string& s)
            {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                                [](unsigned char c) { return !std::isspace(c); }));
                s.erase(std::find_if(s.rbegin(), s.rend(),
                                     [](unsigned char c) { return !std::isspace(c); })
                            .base(),
                        s.end());
            };
            trim(key);
            trim(value);
            if (!key.empty()) state->headers[key] = value;
        }
        return total;
    }

    openai::HttpResponse do_curl(const openai::HttpRequest& request)
    {
        CURL* curl = curl_easy_init();
        if (!curl) throw openai::OpenAIError("Failed to initialize libcurl");

        struct curl_slist* header_list = nullptr;
        for (const auto& [key, value] : request.headers)
        {
            header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
        }

        std::string response_body;
        std::function<void(const char*, std::size_t)> on_chunk = request.on_chunk;
        RequestState state{request.collect_body ? &response_body : nullptr,
                           on_chunk ? &on_chunk : nullptr, this};

        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        if (!request.body.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, request.body.size());
        }

        const CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        if (state.error)
        {
            if (abort_->load())
            {
                throw openai::OpenAIError("Request was aborted");
            }
            throw openai::OpenAIError("Streaming callback failed");
        }
        if (res != CURLE_OK)
        {
            if (abort_->load())
            {
                throw openai::OpenAIError("Request was aborted");
            }
            throw openai::OpenAIError(std::string("libcurl error: ") + curl_easy_strerror(res));
        }
        openai::HttpResponse response;
        response.status_code = state.status;
        response.headers = std::move(state.headers);
        // 流式模式下 collect_body=false：错误响应体仍保留，成功响应体为空
        response.body =
            request.collect_body ? std::move(response_body) : std::move(state.error_body);
        return response;
    }

    std::shared_ptr<std::atomic<bool>> abort_;
    std::function<void(Json&)> augmenter_;
};

std::unique_ptr<openai::HttpClient> OpenAiCompletionsTransport::Impl::make_http_client()
{
    auto abort = abort_;
    auto augmenter = [this](Json& body)
    {
        for (auto& [key, value] : augment_config_.items()) body[key] = value;
        if (body.contains("messages") && body["messages"].is_array())
        {
            size_t assistant_index = 0;
            for (auto& msg : body["messages"])
            {
                if (!msg.is_object()) continue;
                if (msg.value("role", "") == "assistant")
                {
                    // 依赖 ConvertedMessages 的序号对应不变量（见 convert_messages）；
                    // 超出范围时跳过注入而不是错配。
                    if (assistant_index < assistant_extras_.size() &&
                        !assistant_extras_[assistant_index].is_null())
                    {
                        for (auto& [key, value] : assistant_extras_[assistant_index].items())
                            msg[key] = value;
                    }
                    if (!msg.contains("content") && msg.contains("tool_calls"))
                        msg["content"] = nullptr;
                    ++assistant_index;
                }
            }
        }
    };
    return std::make_unique<AbortableHttpClient>(abort, std::move(augmenter));
}

OpenAiCompletionsTransport::OpenAiCompletionsTransport(std::string baseUrl, std::string apiKey,
                                                       std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(std::move(baseUrl), std::move(apiKey), timeout))
{
}

void OpenAiCompletionsTransport::stream_chat(const ModelInfo& model,
                                             const std::vector<Message>& messages,
                                             const StreamRequestOptions& opts,
                                             const std::function<void(const StreamEvent&)>& sink)
{
    impl_->stream_chat(model, messages, opts, sink);
}

std::shared_ptr<TransportAdapter> make_openai_completions_transport(
    std::string baseUrl, std::string apiKey, std::chrono::milliseconds timeout)
{
    return std::make_shared<OpenAiCompletionsTransport>(std::move(baseUrl), std::move(apiKey),
                                                        timeout);
}

}  // namespace pi

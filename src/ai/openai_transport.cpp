#include "pi/ai/openai_transport.h"

#include <cctype>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "pi/ai/curl_http_client.h"
#include "pi/ai/json_util.h"
#include "pi/ai/openai_mini.h"

namespace pi
{

namespace
{

using openai_json = nlohmann::json;

/**
 * 端点兼容开关：只区分 DeepSeek 与通用 OpenAI 兼容端点。
 * 不再维护 zai/moonshot/together/xai/cerebras 等 provider 分支（无内置模型、无测试）。
 */
struct Compat
{
    /** DeepSeek：追加 thinking 参数、回放 reasoning_content、合成缺失的 reasoning 字段。 */
    bool deepseek = false;
};

Compat detect_compat(const ModelInfo& model)
{
    return {model.provider == "deepseek" ||
            model.baseUrl.find("deepseek.com") != std::string::npos};
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
 * 消息转换（镜像 pi 的 transformMessages）：非视觉模型图片降级、thinking 块跨模型处理、
 * 跳过 error/aborted 的 assistant 消息、为孤儿 tool call 补合成 toolResult。
 *
 * 单遍完成：每条消息只拷贝一次（旧实现第一遍转存、第二遍再拷一份）。
 */
std::vector<Message> transform_messages(const std::vector<Message>& messages,
                                        const ModelInfo& model)
{
    std::vector<Message> result;
    result.reserve(messages.size());

    // 待补 synthetic toolResult 的 tool call（指向 result 中已稳定存放的 ContentBlock）
    std::vector<const ContentBlock*> pending_tool_calls;
    std::set<std::string> existing_tool_result_ids;

    const auto insert_synthetic = [&]()
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

    for (const auto& msg : messages)
    {
        if (msg.role == Role::Assistant)
        {
            insert_synthetic();
            if (msg.stopReason == StopReason::Error || msg.stopReason == StopReason::Aborted)
                continue;

            const bool same_model =
                msg.provider == model.provider && msg.api == model.api && msg.model == model.id;
            Message out = msg;
            out.content.clear();
            out.content.reserve(msg.content.size());
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Thinking)
                {
                    if (block.redacted)
                    {
                        if (same_model) out.content.push_back(block);
                        continue;
                    }
                    if (same_model && !block.thinkingSignature.empty())
                    {
                        out.content.push_back(block);
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
                        out.content.push_back(block);
                    }
                    else
                    {
                        ContentBlock text;
                        text.type = BlockType::Text;
                        text.text = block.thinking;
                        out.content.push_back(std::move(text));
                    }
                }
                else if (block.type == BlockType::ToolCall && !same_model)
                {
                    ContentBlock call = block;
                    if (!call.thoughtSignature.empty()) call.thoughtSignature.clear();
                    out.content.push_back(std::move(call));
                }
                else
                {
                    out.content.push_back(block);
                }
            }
            result.push_back(std::move(out));
            pending_tool_calls.clear();
            existing_tool_result_ids.clear();
            for (const auto& block : result.back().content)
            {
                if (block.type == BlockType::ToolCall) pending_tool_calls.push_back(&block);
            }
            continue;
        }

        if (msg.role == Role::User || msg.role == Role::ToolResult)
        {
            if (msg.role == Role::User) insert_synthetic();
            Message out = msg;
            if (!model.supports_images())
            {
                out.content = replace_images_with_placeholder(
                    msg.content, msg.role == Role::User ? kNonVisionUserImagePlaceholder
                                                        : kNonVisionToolImagePlaceholder);
            }
            if (msg.role == Role::ToolResult) existing_tool_result_ids.insert(msg.toolCallId);
            result.push_back(std::move(out));
            continue;
        }

        insert_synthetic();
        result.push_back(msg);
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

/** 消息 → wire 格式（镜像 pi 的 convertMessages）。额外字段挂在 ChatMessage::extra 上。 */
struct ConvertedMessages
{
    std::vector<openai::ChatMessage> messages;
};

ConvertedMessages convert_messages(const ModelInfo& model, const std::vector<Message>& messages,
                                   const std::string& systemPrompt, const Compat& compat)
{
    ConvertedMessages out;

    if (!systemPrompt.empty())
    {
        openai::ChatMessage sys;
        sys.role = "system";
        openai::ChatMessageContent part;
        part.type = openai::ChatMessageContent::Type::Text;
        part.text = systemPrompt;
        sys.content.push_back(std::move(part));
        out.messages.push_back(std::move(sys));
    }

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

            if (compat.deepseek && model.reasoning && !extra.contains("reasoning_content"))
            {
                extra["reasoning_content"] = "";
            }

            const bool has_content = !wire.content.empty() || !assistant_text.empty();
            if (!has_content && wire.tool_calls.empty())
            {
                continue;  // 跳过既无内容又无工具调用的 assistant 消息
            }
            wire.extra = std::move(extra);
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
                out.messages.push_back(std::move(tool));
            }
            i = j - 1;

            if (!image_blocks.empty() && model.supports_images())
            {
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
            }
            continue;
        }
    }

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
        // 客户端与 abort 信号按请求更新。
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
                "Missing API key. Set PI_API_KEY, DEEPSEEK_API_KEY, or OPENAI_API_KEY before "
                "sending a prompt.";
            StreamEvent error;
            error.type = StreamEvent::Type::Error;
            error.reason = StopReason::Error;
            error.message = output;
            sink(error);
            return;
        }
        try
        {
            client_ = make_client(request_api_key, model);
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
        auto converted = convert_messages(model, transformed, systemPrompt, compat);

        // deepseek 的顶层 thinking 字段（直接并进请求体，不做 parse/dump）
        Json augment_config = Json::object();
        if (model.reasoning && compat.deepseek)
        {
            augment_config["thinking"] = Json{
                {"type",
                 opts.reasoning && *opts.reasoning != ThinkingLevel::Off ? "enabled" : "disabled"}};
        }
        openai::ChatCompletionRequest request;
        request.model = model.id;
        request.messages = std::move(converted.messages);
        request.extra = augment_config;
        if (opts.maxTokens) request.max_tokens = *opts.maxTokens;
        if (opts.temperature) request.temperature = *opts.temperature;
        openai::ChatCompletionStreamOptions stream_options;
        stream_options.include_usage = true;
        request.stream_options = stream_options;
        if (opts.reasoning && model.reasoning)
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
            request.tools = convert_tools(opts.tools);
        }
        else if (has_tool_history(messages))
        {
            request.tools = {};
        }

        openai::RequestOptions request_options;
        request_options.max_retries = static_cast<size_t>(opts.maxRetries);
        request_options.timeout = opts.timeoutMs;
        // 不手动注入 Authorization：客户端已用 request_api_key + use_bearer_auth 构造，
        // 内置客户端会自行添加 "Bearer <key>"。这里重复注入会依赖服务端对重复头的处理。
        for (const auto& [key, value] : opts.headers)
        {
            request_options.headers[key] = value;
        }
        // 注意：opts.onResponse（响应头回调）当前未实现——内置 HttpClient 接口
        // 不向上传递原始响应头，状态码/错误信息已通过异常与错误事件携带。
        // 若未来需要，需在 AbortableHttpClient 中把 status/headers 回传到 Impl。
        (void)opts.onResponse;

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
    std::unique_ptr<openai::OpenAIClient> make_client(const std::string& apiKey,
                                                      const ModelInfo& model)
    {
        openai::ClientOptions options;
        options.api_key = apiKey;
        // 自定义模型用自身 baseUrl（settings.json 的 models[]）；内置模型跟随全局
        // baseUrl（settings.baseUrl / PI_BASE_URL），保持既有覆盖语义。
        const bool custom_endpoint = model.provider == "custom" && !model.baseUrl.empty();
        options.base_url = custom_endpoint ? model.baseUrl : baseUrl_;
        options.timeout = timeout_;
        options.max_retries = 0;
        options.use_bearer_auth = true;
        return std::make_unique<openai::OpenAIClient>(options, make_http_client());
    }

    struct StreamContext
    {
        StreamContext(Message& message, const std::function<void(const StreamEvent&)>& event_sink,
                      const ModelInfo& model_info)
            : output(message), sink(event_sink), model(model_info)
        {
        }

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

        // 正常的流结束标记：不是 JSON，也不应计入 malformed chunk。
        if (sse.data == "[DONE]")
        {
            ctx.hasFinishReason = true;
            return false;
        }

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

        // 部分网关/代理在 200 流内用 {"error":{...}} 报错：必须显式转成错误，
        // 否则会被当成"没有 choices 的 chunk"吞掉，最终报成 finish_reason 缺失。
        if (chunk.contains("error"))
        {
            const auto& error = chunk["error"];
            std::string message;
            if (error.is_object() && error.contains("message") && error["message"].is_string())
            {
                message = error["message"].get<std::string>();
            }
            else if (error.is_string())
            {
                message = error.get<std::string>();
            }
            ctx.output.stopReason = StopReason::Error;
            ctx.output.errorMessage =
                message.empty() ? "Provider returned an error chunk" : message;
            return false;
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
            output.usage = parse_chunk_usage(chunk["usage"]);
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
                output.usage = parse_chunk_usage(choice["usage"]);
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
                    // 不随每个 delta 复制整条部分消息（O(n²)）：只有块边界/Done 事件携带 message。
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
                            // 只累积参数；解析放在 finish_block 一次完成（每个 delta 解析是
                            // O(n²)）。
                            ctx.partialArgs[block_index] += delta_text;
                        }
                        StreamEvent event;
                        event.type = StreamEvent::Type::ToolCallDelta;
                        event.contentIndex = block_index;
                        event.delta = delta_text;
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

    static Usage parse_chunk_usage(const Json& raw)
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
        const std::vector<Tool>& tools)
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
            out.push_back(std::move(def));
        }
        return out;
    }

    std::unique_ptr<openai::HttpClient> make_http_client();

    std::string baseUrl_;
    std::string apiKey_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<std::atomic<bool>> abort_;
    std::unique_ptr<openai::OpenAIClient> client_;
    std::mutex stream_mutex_;
};

std::unique_ptr<openai::HttpClient> OpenAiCompletionsTransport::Impl::make_http_client()
{
    return make_curl_http_client(abort_);
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

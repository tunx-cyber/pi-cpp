#pragma once

#include <chrono>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pi/ai/events.h"
#include "pi/ai/model_info.h"

namespace pi
{

/** 传输请求选项，镜像 pi 的 StreamOptions 中我们用到的部分。 */
struct StreamRequestOptions
{
    std::optional<std::string> apiKey;
    std::optional<ThinkingLevel> reasoning;
    std::optional<int> maxTokens;
    std::optional<double> temperature;
    std::map<std::string, std::string> headers;
    std::shared_ptr<std::atomic<bool>> abort;
    int maxRetries = 0;
    std::chrono::milliseconds timeoutMs{600000};
    std::string systemPrompt;
    std::vector<Tool> tools;
    /** 响应头回调（status + headers），镜像 onResponse。 */
    std::function<void(int, const std::map<std::string, std::string>&)> onResponse;
};

/**
 * TransportAdapter 是唯一知道 HTTP/SSE 协议的层。
 * 用户自己的协议库在此对接；默认提供 openai-cpp 实现。
 *
 * 契约（镜像 pi 的 StreamFunction）：
 * - stream_chat 不得抛异常；请求/模型/运行时失败一律以 kError 事件终止
 * - 正常终止为 kDone 事件（stopReason=stop/length/toolUse）
 * - 中断为 kError 事件（stopReason=aborted，errorMessage 描述）
 */
class TransportAdapter
{
   public:
    virtual ~TransportAdapter() = default;

    virtual void stream_chat(const ModelInfo& model, const std::vector<Message>& messages,
                             const StreamRequestOptions& opts,
                             const std::function<void(const StreamEvent&)>& sink) = 0;

    /** 聚合版本：收集事件直至 done/error，返回最终 Message（error 时 stopReason=error/aborted）。
     */
    Message complete_chat(const ModelInfo& model, const std::vector<Message>& messages,
                          const StreamRequestOptions& opts)
    {
        Message result;
        stream_chat(
            model, messages, opts,
            [&result](const StreamEvent& event)
            {
                if (event.type == StreamEvent::Type::Done || event.type == StreamEvent::Type::Error)
                {
                    result = event.message;
                }
            });
        return result;
    }
};

/** 默认 OpenAI-completions 传输工厂（openai-cpp 实现）。 */
std::shared_ptr<TransportAdapter> make_openai_completions_transport(
    std::string baseUrl, std::string apiKey,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(600000));

}  // namespace pi

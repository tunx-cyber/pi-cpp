#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pi/ai/model_info.h"
#include "pi/ai/transport_adapter.h"

namespace pi
{

/**
 * OpenAI-completions 传输实现（基于内置轻量 openai 客户端）。
 * 负责：Message/Tool → wire JSON 的转换（镜像 pi 的 convertMessages/transformMessages）、
 * 流式 SSE 事件 → StreamEvent 的映射、usage/stopReason 解析、abort 传播。
 */
class OpenAiCompletionsTransport : public TransportAdapter
{
   public:
    OpenAiCompletionsTransport(
        std::string baseUrl, std::string apiKey,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(600000));

    void stream_chat(const ModelInfo& model, const std::vector<Message>& messages,
                     const StreamRequestOptions& opts,
                     const std::function<void(const StreamEvent&)>& sink) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pi

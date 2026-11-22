#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/types.h"
#include "pi/ai/transport_adapter.h"

namespace pi
{

/** agent 循环配置，镜像 pi 的 AgentLoopConfig（streamFn → transport 直连）。 */
struct AgentLoopConfig
{
    ModelInfo model;
    std::optional<ThinkingLevel> reasoning;
    std::shared_ptr<TransportAdapter> transport;
    std::function<std::optional<std::string>(const std::string& provider)> getApiKey;
    std::function<bool(const ShouldStopAfterTurnContext&)> shouldStopAfterTurn;
    std::function<std::vector<AgentMessage>()> getSteeringMessages;
};

/**
 * 启动一个 agent 循环（新增 prompt）。镜像 pi 的 runAgentLoop。
 * context 会被追加 prompt 与循环产生的消息（调用方持有）。
 * 返回本次循环新增的消息（含 prompt）。
 */
std::vector<AgentMessage> run_agent_loop(const std::vector<AgentMessage>& prompts,
                                         AgentContext& context, AgentLoopConfig& config,
                                         const AgentEventSink& emit,
                                         const std::shared_ptr<std::atomic<bool>>& signal);

}  // namespace pi

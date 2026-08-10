#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/types.h"
#include "pi/ai/model_info.h"
#include "pi/ai/transport_adapter.h"

namespace pi
{

/** 子 agent 工具配置。 */
struct SubagentToolOptions
{
    ModelInfo model;
    std::optional<ThinkingLevel> thinkingLevel;
    std::string systemPrompt;
    std::vector<AgentTool> tools;
    std::shared_ptr<TransportAdapter> transport;
    std::function<std::optional<std::string>(const std::string& provider)> getApiKey;
    int maxTurns = 20;
    int maxNestingDepth = 2;
};

/**
 * 子 agent 工具：在父工具线程上内嵌独立 Agent，结果包成 <subagent> 文本块。
 * 子 agent 流式输出转发为 tool_execution_update；共享 abort；深度/轮次防护。
 */
AgentTool make_subagent_tool(SubagentToolOptions options);

}  // namespace pi

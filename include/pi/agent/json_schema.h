#pragma once

#include <optional>
#include <string>

#include "pi/agent/types.h"
#include "pi/ai/types.h"

namespace pi
{

/** 校验工具参数；失败返回错误描述，成功返回 nullopt。 */
std::optional<std::string> validate_tool_args(const AgentTool& tool, const Json& args);

/**
 * 容错解析工具调用参数（parseStreamingJson + prepareArguments shim 兜底）。
 * 返回解析后的 Json（失败时尽力而为）。
 */
Json parse_tool_args(const AgentTool& tool, const std::string& rawArgs);

}  // namespace pi

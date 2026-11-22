#pragma once

#include <optional>
#include <string>

#include "pi/agent/types.h"
#include "pi/ai/types.h"

namespace pi
{

/** 校验工具参数；失败返回错误描述，成功返回 nullopt。 */
std::optional<std::string> validate_tool_args(const AgentTool& tool, const Json& args);

}  // namespace pi

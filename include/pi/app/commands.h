#pragma once

#include <string>
#include <vector>

#include "pi/agent/types.h"

namespace pi
{

/** 编码工具（read/bash/edit/write/grep/find/ls/web_fetch），供 agent 使用。 */
std::vector<AgentTool> make_coding_tools(const std::string& cwd);

}  // namespace pi

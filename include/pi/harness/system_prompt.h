#pragma once

#include <string>

#include "pi/harness/types.h"

namespace pi
{

/** system prompt 组装：base + skills XML + 可选附加上下文。 */
std::string build_system_prompt(const std::string& basePrompt, const std::vector<Skill>& skills,
                                const std::string& extraContext = "");

}  // namespace pi

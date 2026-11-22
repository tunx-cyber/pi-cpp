#pragma once

#include <string>
#include <vector>

#include "pi/agent/types.h"
#include "pi/app/theme.h"

namespace pi
{

/**
 * 把会话消息格式化为可回放的文本（/resume 与 --resume 启动时使用）。
 * 只做展示：用户/助手文本、工具调用、工具结果；单条超长内容会截断。
 */
std::string format_conversation(const std::vector<AgentMessage>& messages, const Theme& theme);

/** 回放时的单条展示上限（超出追加 "...(truncated)"）。 */
inline constexpr size_t kConversationMaxChars = 2000;

}  // namespace pi

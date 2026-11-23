#pragma once

#include <string>

#include "pi/ai/types.h"

namespace pi
{

/**
 * 运行配置：只从环境变量解析（进程环境 → 项目 `.env` → `~/.pi-cpp/.env`）。
 *
 * 不读写 `~/.pi-cpp/settings.json`：模型与 thinking 级别随会话（JSONL 里的
 * ModelChange / ThinkingLevelChange 条目）保存，全局默认值一律由环境变量给出。
 */
struct Settings
{
    /** `PI_API_KEY` > `OPENAI_API_KEY`。 */
    std::string apiKey;
    /** `PI_BASE_URL`；裸域名自动补 `/v1`。 */
    std::string baseUrl = "https://api.deepseek.com";
    /** `PI_MODEL`；空表示"未指定"，由端点 `GET /models` 决定。 */
    std::string model;
    /** `PI_THINKING`（off/minimal/low/medium/high/xhigh）。 */
    ThinkingLevel thinking = ThinkingLevel::Off;
    /** `PI_SESSIONS_ROOT`。 */
    std::string sessionsRoot = "~/.pi-cpp/agent/sessions";
    /** `PI_SYSTEM_PROMPT`：覆盖内置的 base 系统提示词。 */
    std::string systemPrompt = "You are pi-cpp, a helpful coding assistant in a terminal.";
    /** `PI_THEME`（default / mono）。 */
    std::string theme = "default";

    /** 读取配置（缺失的变量用结构体里的默认值）。 */
    static Settings load();

    /** 展开 `~`。 */
    static std::string expand_home(const std::string& path);
};

}  // namespace pi

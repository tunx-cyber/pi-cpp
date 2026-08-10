#pragma once

#include <optional>
#include <string>

#include "pi/ai/types.h"

namespace pi
{

/** ~/.pi-cpp/settings.json（模型/thinking 按 cwd 持久化）。 */
struct Settings
{
    std::string apiKey;  // 优先级低于 env DEEPSEEK_API_KEY / PI_API_KEY
    std::string baseUrl = "https://api.deepseek.com";
    std::string model = "deepseek-v4-flash";
    ThinkingLevel thinking = ThinkingLevel::Off;
    std::string sessionsRoot = "~/.pi-cpp/agent/sessions";

    /** 按 cwd 覆盖（模型选择跨重启保留）。 */
    struct CwdOverride
    {
        std::optional<std::string> model;
        std::optional<ThinkingLevel> thinking;
    };
    std::optional<CwdOverride> override_for(const std::string& cwd) const;

    /** 加载（文件不存在返回默认值）。 */
    static Settings load();
    /** 保存（合并 cwd 覆盖后写回）。 */
    void save() const;
    /** 更新某 cwd 的覆盖并持久化。 */
    void set_override(const std::string& cwd, const std::optional<std::string>& model,
                      const std::optional<ThinkingLevel>& thinking);

    /** 展开 ~ 并解析。 */
    static std::string expand_home(const std::string& path);
};

}  // namespace pi

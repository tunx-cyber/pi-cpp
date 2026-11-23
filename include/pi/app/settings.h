#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pi/ai/model_info.h"
#include "pi/ai/types.h"
#include "pi/app/commands.h"
#include "pi/harness/types.h"

namespace pi
{

/** ~/.pi-cpp/settings.json（模型/thinking 按 cwd 持久化）。 */
struct Settings
{
    /** 仅从 env 解析（PI_API_KEY / OPENAI_API_KEY），不读 settings.json。 */
    std::string apiKey;
    /** 代码默认端点；可用 env PI_BASE_URL 覆盖，不读 settings.json。 */
    std::string baseUrl = "https://api.deepseek.com";
    /**
     * 当前模型 id；空表示"未指定"——运行时由端点 `GET /models` 返回的第一个模型确定，
     * 不在代码里写死任何默认模型。可由 PI_MODEL / settings.json / byCwd 显式指定。
     */
    std::string model;
    ThinkingLevel thinking = ThinkingLevel::Off;
    std::string sessionsRoot = "~/.pi-cpp/agent/sessions";
    /** 系统提示词 base（settings.json 的 systemPrompt 键）。 */
    std::string systemPrompt = "You are pi-cpp, a helpful coding assistant in a terminal.";
    /** settings.json 的 models 数组定义的自定义模型（可覆盖内置模型）。 */
    std::vector<ModelInfo> models;
    /** 自动压缩参数（settings.json 的 compaction 对象）。 */
    CompactionSettings compaction;
    /** web_search 参数（settings.json 的 webSearch 对象）。 */
    WebSearchConfig webSearch;
    /** 终端主题（settings.json 的 theme 键，默认 default；预留自定义）。 */
    std::string theme = "default";

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

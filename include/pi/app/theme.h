#pragma once

#include <string>

namespace pi
{

/**
 * 终端主题：颜色集中在这里，便于以后扩展（用 PI_THEME 选择，预留自定义主题）。
 * 目前只有 default（彩色）与 mono（无颜色，NO_COLOR 时自动选用）。
 */
struct Theme
{
    std::string name = "default";
    std::string prompt;     // 提示符
    std::string user;       // 用户消息（回放）
    std::string assistant;  // 助手消息（回放）
    std::string thinking;   // 思考内容（弱化）
    std::string tool;       // 工具调用 / 结果
    std::string error;      // 错误
    std::string dim;        // 次要信息（状态栏、提示）
    std::string reset = "\033[0m";

    bool colored() const { return !reset.empty(); }
};

/** 彩色主题：青=提示符/用户，黄=工具，红=错误，暗=次要信息。 */
inline Theme make_default_theme()
{
    Theme theme;
    theme.prompt = "\033[36m";
    theme.user = "\033[36m";
    theme.assistant = "\033[0m";
    theme.thinking = "\033[2m";
    theme.tool = "\033[33m";
    theme.error = "\033[31m";
    theme.dim = "\033[2m";
    return theme;
}

/** 无颜色主题（NO_COLOR 或 theme=mono）。 */
inline Theme make_mono_theme()
{
    Theme theme;
    theme.name = "mono";
    theme.reset.clear();
    return theme;
}

/** 按名称取内置主题；未知名称回退 default。 */
inline const Theme& theme_by_name(const std::string& name)
{
    static const Theme kDefault = make_default_theme();
    static const Theme kMono = make_mono_theme();
    if (name == "mono") return kMono;
    return kDefault;
}

/** 用主题颜色包裹文本；mono 主题下原样返回。 */
inline std::string styled(const Theme& theme, const std::string& color, const std::string& text)
{
    if (color.empty() || !theme.colored()) return text;
    return color + text + theme.reset;
}

}  // namespace pi

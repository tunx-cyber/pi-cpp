#include "pi/app/settings.h"

#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <map>

namespace pi
{

std::string Settings::expand_home(const std::string& path)
{
    if (path.empty() || path.front() != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home) return path;
    if (path.size() == 1) return home;
    return std::string(home) + path.substr(1);
}

namespace
{

/** 解析 .env 文件（KEY=VALUE 行，支持 # 注释与引号）。 */
std::map<std::string, std::string> load_env_file(const std::string& path)
{
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    if (!file) return values;
    std::string line;
    while (std::getline(file, line))
    {
        // 去首尾空白
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r");
        line = line.substr(first, last - first + 1);
        if (line.empty() || line.front() == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string value = line.substr(eq + 1);
        const size_t v_first = value.find_first_not_of(" \t");
        value = v_first == std::string::npos ? "" : value.substr(v_first);
        const size_t v_last = value.find_last_not_of(" \t\r");
        value = value.substr(0, v_last + 1);
        // 去掉配对引号
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }
        values[key] = value;
    }
    return values;
}

/** 从当前目录向上找最近的 .env（从 build/ 里启动时也能找到项目根）。 */
std::string find_project_env_file()
{
    std::error_code ec;
    auto directory = std::filesystem::current_path(ec);
    if (ec) return ".env";
    while (true)
    {
        const auto candidate = directory / ".env";
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) return candidate.string();
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return ".env";
}

/**
 * 端点地址规范化：去掉尾部斜杠；只有 `scheme://host[:port]`（没有路径）时按 OpenAI
 * 兼容惯例补 `/v1`。例如 PI_BASE_URL=https://api.xiaomimimo.com → https://api.xiaomimimo.com/v1，
 * 否则 `/models` 与 `/chat/completions` 会 404。带路径的地址（…/openai/v1、…/api）原样保留。
 */
std::string normalize_base_url(std::string url)
{
    while (!url.empty() && url.back() == '/') url.pop_back();
    const size_t scheme = url.find("://");
    const size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
    if (url.find('/', host_start) == std::string::npos) url += "/v1";
    return url;
}

}  // namespace

Settings Settings::load()
{
    Settings settings;
    // 优先级：进程环境变量 > 项目 .env（向上找最近）> ~/.pi-cpp/.env > 结构体默认值
    const auto env_file = load_env_file(expand_home("~/.pi-cpp/.env"));
    const auto project_env = load_env_file(find_project_env_file());
    auto get_env_value = [&](const std::string& key) -> std::string
    {
        if (const char* value = std::getenv(key.c_str()); value && *value) return value;
        if (project_env.count(key)) return project_env.at(key);
        if (env_file.count(key)) return env_file.at(key);
        return "";
    };

    settings.apiKey = get_env_value("PI_API_KEY");
    if (settings.apiKey.empty()) settings.apiKey = get_env_value("OPENAI_API_KEY");

    const std::string base_url = get_env_value("PI_BASE_URL");
    if (!base_url.empty()) settings.baseUrl = base_url;
    settings.baseUrl = normalize_base_url(settings.baseUrl);

    settings.model = get_env_value("PI_MODEL");
    if (const auto level = thinking_level_from_string(get_env_value("PI_THINKING")))
    {
        settings.thinking = *level;
    }
    const std::string sessions_root = get_env_value("PI_SESSIONS_ROOT");
    if (!sessions_root.empty()) settings.sessionsRoot = sessions_root;
    const std::string system_prompt = get_env_value("PI_SYSTEM_PROMPT");
    if (!system_prompt.empty()) settings.systemPrompt = system_prompt;
    const std::string theme = get_env_value("PI_THEME");
    if (!theme.empty()) settings.theme = theme;
    return settings;
}

}  // namespace pi

#include "pi/app/settings.h"

#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

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

std::optional<Json> load_json_file(const std::string& path)
{
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    try
    {
        return Json::parse(buffer.str());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<ThinkingLevel> thinking_from_json(const Json& json)
{
    if (!json.is_string()) return std::nullopt;
    return thinking_level_from_string(json.get<std::string>());
}

std::string env_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
}

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

}  // namespace

std::optional<Settings::CwdOverride> Settings::override_for(const std::string& cwd) const
{
    std::optional<CwdOverride> result;
    const auto json = load_json_file(expand_home("~/.pi-cpp/settings.json"));
    if (!json || !json->contains("byCwd") || !(*json)["byCwd"].is_object()) return result;
    const auto it = (*json)["byCwd"].find(cwd);
    if (it == (*json)["byCwd"].end() || !it->is_object()) return result;

    CwdOverride override;
    if (it->contains("model")) override.model = (*it)["model"].get<std::string>();
    if (it->contains("thinking")) override.thinking = thinking_from_json((*it)["thinking"]);
    return override;
}

Settings Settings::load()
{
    Settings settings;
    const std::string path = expand_home("~/.pi-cpp/settings.json");
    const auto json = load_json_file(path);
    if (json)
    {
        if (json->contains("apiKey") && (*json)["apiKey"].is_string())
        {
            settings.apiKey = (*json)["apiKey"].get<std::string>();
        }
        if (json->contains("baseUrl") && (*json)["baseUrl"].is_string())
        {
            settings.baseUrl = (*json)["baseUrl"].get<std::string>();
        }
        if (json->contains("model") && (*json)["model"].is_string())
        {
            settings.model = (*json)["model"].get<std::string>();
        }
        if (json->contains("thinking"))
        {
            if (const auto level = thinking_from_json((*json)["thinking"]))
                settings.thinking = *level;
        }
    }
    // .env 文件（优先级：~/.pi-cpp/.env < 项目根 .env < 环境变量）
    const auto env_file = load_env_file(expand_home("~/.pi-cpp/.env"));
    const auto project_env = load_env_file(".env");
    auto get_env_value = [&](const std::string& key) -> std::string
    {
        if (const char* value = std::getenv(key.c_str()); value && *value) return value;
        if (project_env.count(key)) return project_env.at(key);
        if (env_file.count(key)) return env_file.at(key);
        return "";
    };
    const std::string env_key = get_env_value("PI_API_KEY");
    const std::string deepseek_key = get_env_value("DEEPSEEK_API_KEY");
    if (!env_key.empty()) settings.apiKey = env_key;
    if (settings.apiKey.empty() && !deepseek_key.empty()) settings.apiKey = deepseek_key;
    // env 覆盖端点与模型（便于切换/测试）
    const std::string env_base = get_env_value("PI_BASE_URL");
    if (!env_base.empty()) settings.baseUrl = env_base;
    const std::string env_model = get_env_value("PI_MODEL");
    if (!env_model.empty()) settings.model = env_model;
    return settings;
}

void Settings::save() const
{
    const std::string path = expand_home("~/.pi-cpp/settings.json");
    const std::string dir = path.substr(0, path.find_last_of('/'));
    std::filesystem::create_directories(dir);

    Json json = Json::object();
    if (!apiKey.empty()) json["apiKey"] = apiKey;
    json["baseUrl"] = baseUrl;
    json["model"] = model;
    json["thinking"] = to_string(thinking);

    const auto existing = load_json_file(path);
    if (existing && existing->contains("byCwd") && (*existing)["byCwd"].is_object())
    {
        json["byCwd"] = (*existing)["byCwd"];
    }
    else
    {
        json["byCwd"] = Json::object();
    }
    std::ofstream file(path);
    if (file) file << json.dump(2) << std::endl;
}

void Settings::set_override(const std::string& cwd, const std::optional<std::string>& modelOverride,
                            const std::optional<ThinkingLevel>& thinkingOverride)
{
    Json by_cwd = Json::object();
    const auto existing = load_json_file(expand_home("~/.pi-cpp/settings.json"));
    if (existing && existing->contains("byCwd") && (*existing)["byCwd"].is_object())
    {
        by_cwd = (*existing)["byCwd"];
    }
    Json entry = Json::object();
    if (by_cwd.contains(cwd) && by_cwd[cwd].is_object()) entry = by_cwd[cwd];
    if (modelOverride)
    {
        entry["model"] = *modelOverride;
    }
    else
    {
        entry.erase("model");
    }
    if (thinkingOverride)
    {
        entry["thinking"] = to_string(*thinkingOverride);
    }
    else
    {
        entry.erase("thinking");
    }
    if (entry.empty())
    {
        by_cwd.erase(cwd);
    }
    else
    {
        by_cwd[cwd] = entry;
    }
    save();
}

}  // namespace pi

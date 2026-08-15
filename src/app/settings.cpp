#include "pi/app/settings.h"

#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

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

bool write_settings_json(const std::string& path, const Json& json)
{
    const std::string dir = path.substr(0, path.find_last_of('/'));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    const std::string temporary = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    {
        std::ofstream file(temporary, std::ios::out | std::ios::trunc);
        if (!file) return false;
        file << json.dump(2) << '\n';
        file.flush();
        if (!file.good())
        {
            std::remove(temporary.c_str());
            return false;
        }
    }
    // Settings contain credentials; do not leave a world-readable file behind.
    if (chmod(temporary.c_str(), 0600) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return chmod(path.c_str(), 0600) == 0;
}

/** 序列化自定义模型（settings.json 的 models 数组元素）。 */
Json model_to_json(const ModelInfo& m)
{
    Json j = Json::object();
    j["id"] = m.id;
    j["name"] = m.name;
    j["baseUrl"] = m.baseUrl;
    j["provider"] = m.provider;
    j["reasoning"] = m.reasoning;
    j["contextWindow"] = m.contextWindow;
    j["maxTokens"] = m.maxTokens;
    j["costInput"] = m.costInput;
    j["costOutput"] = m.costOutput;
    j["costCacheRead"] = m.costCacheRead;
    j["costCacheWrite"] = m.costCacheWrite;
    return j;
}

/** 解析自定义模型；缺省 baseUrl 回退到全局 baseUrl（镜像 make_custom_model 的默认值）。 */
ModelInfo model_from_json(const Json& j, const std::string& defaultBaseUrl)
{
    ModelInfo m;
    m.id = j.value("id", "");
    m.name = j.value("name", m.id);
    m.api = "openai-completions";
    m.provider = j.value("provider", "custom");
    m.baseUrl = j.value("baseUrl", defaultBaseUrl);
    m.reasoning = j.value("reasoning", false);
    m.input = {"text", "image"};
    m.contextWindow = j.value("contextWindow", 131072LL);
    m.maxTokens = j.value("maxTokens", 32768LL);
    m.costInput = j.value("costInput", 0.0);
    m.costOutput = j.value("costOutput", 0.0);
    m.costCacheRead = j.value("costCacheRead", 0.0);
    m.costCacheWrite = j.value("costCacheWrite", 0.0);
    return m;
}

Json settings_json(const Settings& settings, const Json& by_cwd)
{
    Json json = Json::object();
    if (!settings.apiKey.empty()) json["apiKey"] = settings.apiKey;
    json["baseUrl"] = settings.baseUrl;
    json["model"] = settings.model;
    json["thinking"] = to_string(settings.thinking);
    json["sessionsRoot"] = settings.sessionsRoot;
    json["systemPrompt"] = settings.systemPrompt;
    if (settings.pricing.input) json["costInput"] = *settings.pricing.input;
    if (settings.pricing.output) json["costOutput"] = *settings.pricing.output;
    if (settings.pricing.cacheRead) json["costCacheRead"] = *settings.pricing.cacheRead;
    if (settings.pricing.cacheWrite) json["costCacheWrite"] = *settings.pricing.cacheWrite;
    json["compaction"] = Json{{"enabled", settings.compaction.enabled},
                              {"reserveTokens", settings.compaction.reserveTokens},
                              {"keepRecentTokens", settings.compaction.keepRecentTokens}};
    json["webSearch"] = Json{{"baseUrl", settings.webSearch.baseUrl},
                             {"model", settings.webSearch.model},
                             {"maxTokens", settings.webSearch.maxTokens},
                             {"maxUses", settings.webSearch.maxUses},
                             {"maxResults", settings.webSearch.maxResults}};
    if (!settings.models.empty())
    {
        Json arr = Json::array();
        for (const auto& m : settings.models) arr.push_back(model_to_json(m));
        json["models"] = std::move(arr);
    }
    json["byCwd"] = by_cwd;
    return json;
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
        const auto read_price = [&](const char* key) -> std::optional<double>
        {
            if (json->contains(key) && (*json)[key].is_number())
                return (*json)[key].get<double>();
            return std::nullopt;
        };
        settings.pricing.input = read_price("costInput");
        settings.pricing.output = read_price("costOutput");
        settings.pricing.cacheRead = read_price("costCacheRead");
        settings.pricing.cacheWrite = read_price("costCacheWrite");
        if (json->contains("sessionsRoot") && (*json)["sessionsRoot"].is_string())
            settings.sessionsRoot = (*json)["sessionsRoot"].get<std::string>();
        if (json->contains("systemPrompt") && (*json)["systemPrompt"].is_string())
            settings.systemPrompt = (*json)["systemPrompt"].get<std::string>();
        if (json->contains("models") && (*json)["models"].is_array())
        {
            for (const auto& item : (*json)["models"])
            {
                try
                {
                    ModelInfo m = model_from_json(item, settings.baseUrl);
                    if (!m.id.empty()) settings.models.push_back(std::move(m));
                }
                catch (...)
                {
                    // 跳过非法模型条目，避免一个坏条目拖垮整个配置
                }
            }
        }
        if (json->contains("compaction") && (*json)["compaction"].is_object())
        {
            const auto& c = (*json)["compaction"];
            if (c.contains("enabled") && c["enabled"].is_boolean())
                settings.compaction.enabled = c["enabled"].get<bool>();
            if (c.contains("reserveTokens") && c["reserveTokens"].is_number())
                settings.compaction.reserveTokens = c["reserveTokens"].get<int64_t>();
            if (c.contains("keepRecentTokens") && c["keepRecentTokens"].is_number())
                settings.compaction.keepRecentTokens = c["keepRecentTokens"].get<int64_t>();
        }
        if (json->contains("webSearch") && (*json)["webSearch"].is_object())
        {
            const auto& w = (*json)["webSearch"];
            if (w.contains("baseUrl") && w["baseUrl"].is_string())
                settings.webSearch.baseUrl = w["baseUrl"].get<std::string>();
            if (w.contains("model") && w["model"].is_string())
                settings.webSearch.model = w["model"].get<std::string>();
            if (w.contains("maxTokens") && w["maxTokens"].is_number())
                settings.webSearch.maxTokens = w["maxTokens"].get<int>();
            if (w.contains("maxUses") && w["maxUses"].is_number())
                settings.webSearch.maxUses = w["maxUses"].get<int>();
            if (w.contains("maxResults") && w["maxResults"].is_number())
                settings.webSearch.maxResults = w["maxResults"].get<int>();
        }
    }
    // .env 文件（优先级：~/.pi-cpp/.env < 当前目录向上最近的项目 .env < 环境变量）。
    // 从 build/ 目录启动时仍能找到仓库根目录的 .env。
    const auto env_file = load_env_file(expand_home("~/.pi-cpp/.env"));
    const auto project_env = load_env_file(find_project_env_file());
    auto get_env_value = [&](const std::string& key) -> std::string
    {
        if (const char* value = std::getenv(key.c_str()); value && *value) return value;
        if (project_env.count(key)) return project_env.at(key);
        if (env_file.count(key)) return env_file.at(key);
        return "";
    };
    const std::string env_key = get_env_value("PI_API_KEY");
    const std::string deepseek_key = get_env_value("DEEPSEEK_API_KEY");
    const std::string openai_key = get_env_value("OPENAI_API_KEY");
    if (!env_key.empty()) settings.apiKey = env_key;
    if (settings.apiKey.empty() && !deepseek_key.empty()) settings.apiKey = deepseek_key;
    if (settings.apiKey.empty() && !openai_key.empty()) settings.apiKey = openai_key;
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
    Json by_cwd = Json::object();
    const auto existing = load_json_file(path);
    if (existing && existing->contains("byCwd") && (*existing)["byCwd"].is_object())
        by_cwd = (*existing)["byCwd"];
    (void)write_settings_json(path, settings_json(*this, by_cwd));
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
    const std::string path = expand_home("~/.pi-cpp/settings.json");
    (void)write_settings_json(path, settings_json(*this, by_cwd));
}

}  // namespace pi

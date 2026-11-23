#include "pi/app/settings.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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
    // settings.json 不再保存 apiKey/baseUrl；仍保持 0600，避免旧版本遗留的凭据字段被他人读取。
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
    return m;
}

Json settings_json(const Settings& settings, const Json& by_cwd)
{
    Json json = Json::object();
    // 全局 apiKey / baseUrl 只来自 env 与代码默认值，不写入 settings.json。
    // model 为空表示"由端点自动确定"，此时不落盘（避免把某次探测结果固化成默认值）。
    if (!settings.model.empty()) json["model"] = settings.model;
    json["thinking"] = to_string(settings.thinking);
    json["sessionsRoot"] = settings.sessionsRoot;
    json["systemPrompt"] = settings.systemPrompt;
    json["theme"] = settings.theme;
    // 自动压缩已移除：compaction 仅用于手动 /compact 的切点参数。
    json["compaction"] = Json{{"reserveTokens", settings.compaction.reserveTokens},
                              {"keepRecentTokens", settings.compaction.keepRecentTokens}};
    json["webSearch"] = Json{{"baseUrl", settings.webSearch.baseUrl},
                             {"model", settings.webSearch.model},
                             {"maxTokens", settings.webSearch.maxTokens},
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
    // 手写配置里的类型错误不应让启动直接抛异常（其余字段都做了类型检查）
    if (it->contains("model") && (*it)["model"].is_string())
        override.model = (*it)["model"].get<std::string>();
    if (it->contains("thinking")) override.thinking = thinking_from_json((*it)["thinking"]);
    return override;
}

Settings Settings::load()
{
    Settings settings;
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
    // apiKey 与全局端点只来自 env 与 settings.h 的默认值。先应用 env，
    // 让 settings.json 中未显式给 baseUrl 的自定义模型回退到实际端点。
    const std::string env_key = get_env_value("PI_API_KEY");
    const std::string openai_key = get_env_value("OPENAI_API_KEY");
    if (!env_key.empty()) settings.apiKey = env_key;
    if (settings.apiKey.empty() && !openai_key.empty()) settings.apiKey = openai_key;
    const std::string env_base = get_env_value("PI_BASE_URL");
    if (!env_base.empty()) settings.baseUrl = env_base;
    // 裸域名补 /v1（默认端点同样处理），自定义模型随后继承规范化后的默认端点
    settings.baseUrl = normalize_base_url(settings.baseUrl);
    const std::string env_model = get_env_value("PI_MODEL");

    const std::string path = expand_home("~/.pi-cpp/settings.json");
    const auto json = load_json_file(path);
    if (json)
    {
        // apiKey / baseUrl 不从 settings.json 读取（只认 env 与 settings.h 的默认值）；
        // 旧文件中的这两个键会被忽略，并在下次 save() 时自然移除。
        if (json->contains("model") && (*json)["model"].is_string())
        {
            settings.model = (*json)["model"].get<std::string>();
        }
        if (json->contains("thinking"))
        {
            if (const auto level = thinking_from_json((*json)["thinking"]))
                settings.thinking = *level;
        }
        if (json->contains("sessionsRoot") && (*json)["sessionsRoot"].is_string())
            settings.sessionsRoot = (*json)["sessionsRoot"].get<std::string>();
        if (json->contains("systemPrompt") && (*json)["systemPrompt"].is_string())
            settings.systemPrompt = (*json)["systemPrompt"].get<std::string>();
        if (json->contains("theme") && (*json)["theme"].is_string())
            settings.theme = (*json)["theme"].get<std::string>();
        if (json->contains("models") && (*json)["models"].is_array())
        {
            for (const auto& item : (*json)["models"])
            {
                try
                {
                    ModelInfo m = model_from_json(item, settings.baseUrl);
                    if (m.baseUrl.empty()) m.baseUrl = settings.baseUrl;
                    m.baseUrl = normalize_base_url(m.baseUrl);
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
            if (w.contains("maxResults") && w["maxResults"].is_number())
                settings.webSearch.maxResults = w["maxResults"].get<int>();
        }
    }
    // 模型也允许 env 覆盖 settings.json（便于切换/测试）
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

#include "pi/ai/model_registry.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace pi
{

namespace
{

// 镜像 pi models.generated.ts 中 deepseek provider 的两个模型。
// 仅提供元数据（上下文窗口 / maxTokens / thinking 映射）：代码里没有"默认模型"，
// 默认模型由端点 GET /models 决定（见 AgentSession::ensure_model）。
ModelInfo deepseek_v4_flash()
{
    ModelInfo m;
    m.id = "deepseek-v4-flash";
    m.name = "DeepSeek V4 Flash";
    m.api = "openai-completions";
    m.provider = "deepseek";
    m.baseUrl = "https://api.deepseek.com";
    m.reasoning = true;
    m.thinkingLevelMap = {
        {ThinkingLevel::Minimal, std::nullopt}, {ThinkingLevel::Low, std::nullopt},
        {ThinkingLevel::Medium, std::nullopt},  {ThinkingLevel::High, "high"},
        {ThinkingLevel::Xhigh, "max"},
    };
    m.input = {"text"};
    m.contextWindow = 1000000;
    m.maxTokens = 384000;
    return m;
}

ModelInfo deepseek_v4_pro()
{
    ModelInfo m = deepseek_v4_flash();
    m.id = "deepseek-v4-pro";
    m.name = "DeepSeek V4 Pro";
    return m;
}

std::vector<ModelInfo> build_builtin_models() { return {deepseek_v4_flash(), deepseek_v4_pro()}; }

std::vector<ModelInfo>& models()
{
    static std::vector<ModelInfo> models = build_builtin_models();
    return models;
}

/** 模型表互斥锁：register/unregister/get 可能在设置加载与 run 线程之间并发调用。 */
std::mutex& registry_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string ascii_lower(std::string value)
{
    for (char& c : value)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

size_t common_prefix_length(const std::string& a, const std::string& b)
{
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
}

/** 提取 URL 的 host（去掉 scheme / userinfo / 端口 / 路径）。 */
std::string endpoint_host(const std::string& url)
{
    std::string rest = url;
    if (const auto scheme = rest.find("://"); scheme != std::string::npos)
    {
        rest = rest.substr(scheme + 3);
    }
    if (const auto slash = rest.find_first_of("/?#"); slash != std::string::npos)
    {
        rest = rest.substr(0, slash);
    }
    if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[')  // IPv6 字面量：[::1]:8080
    {
        const auto close = rest.find(']');
        if (close != std::string::npos) return ascii_lower(rest.substr(0, close + 1));
    }
    if (const auto colon = rest.find(':'); colon != std::string::npos) rest = rest.substr(0, colon);
    return ascii_lower(rest);
}

}  // namespace

void register_model(const ModelInfo& model)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& list = models();
    for (auto& existing : list)
    {
        if (existing.id == model.id)
        {
            existing = model;
            return;
        }
    }
    list.push_back(model);
}

void unregister_model(const std::string& id)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& list = models();
    list.erase(
        std::remove_if(list.begin(), list.end(), [&](const ModelInfo& m) { return m.id == id; }),
        list.end());
}

std::optional<ModelInfo> get_model(const std::string& modelId)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    for (const auto& model : models())
    {
        if (model.id == modelId) return model;
    }
    return std::nullopt;
}

std::vector<std::string> get_providers()
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::vector<std::string> providers;
    for (const auto& model : models())
    {
        if (std::find(providers.begin(), providers.end(), model.provider) == providers.end())
        {
            providers.push_back(model.provider);
        }
    }
    return providers;
}

std::vector<ModelInfo> get_models(const std::string& provider)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::vector<ModelInfo> out;
    for (const auto& model : models())
    {
        if (model.provider == provider) out.push_back(model);
    }
    return out;
}

std::optional<ModelInfo> find_model_template(const std::string& modelId, const std::string& baseUrl)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    const auto& list = models();
    const std::string id = ascii_lower(modelId);

    // 1) id 前缀与 provider 一致：deepseek-flash / deepseek-v4-pro-2025 → deepseek 家族。
    //    多个候选时取与目标 id 公共前缀最长的一个，尽量贴近具体型号的能力。
    const ModelInfo* best = nullptr;
    size_t best_shared = 0;
    for (const auto& model : list)
    {
        const std::string provider = ascii_lower(model.provider);
        if (provider.empty() || id.compare(0, provider.size(), provider) != 0) continue;
        if (id.size() > provider.size())
        {
            const char separator = id[provider.size()];
            if (separator != '-' && separator != '_' && separator != '/' && separator != ':')
            {
                continue;  // deepseekXYZ 不算同一家族
            }
        }
        const size_t shared = common_prefix_length(id, ascii_lower(model.id));
        if (best == nullptr || shared > best_shared)
        {
            best = &model;
            best_shared = shared;
        }
    }
    if (best != nullptr) return *best;

    // 2) 端点 host 一致：同一服务商的模型共享同一套能力（如 api.deepseek.com）
    const std::string host = endpoint_host(baseUrl);
    if (!host.empty())
    {
        for (const auto& model : list)
        {
            if (endpoint_host(model.baseUrl) == host) return model;
        }
    }
    return std::nullopt;
}

bool models_are_equal(const ModelInfo& a, const ModelInfo& b)
{
    return a.id == b.id && a.provider == b.provider;
}

}  // namespace pi

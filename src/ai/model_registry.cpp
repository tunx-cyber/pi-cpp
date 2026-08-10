#include "pi/ai/model_registry.h"

#include <algorithm>
#include <map>

namespace pi
{

namespace
{

// 镜像 pi models.generated.ts 中 deepseek provider 的两个模型
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
    m.costInput = 0.14;
    m.costOutput = 0.28;
    m.costCacheRead = 0.0028;
    m.costCacheWrite = 0;
    m.contextWindow = 1000000;
    m.maxTokens = 384000;
    return m;
}

ModelInfo deepseek_v4_pro()
{
    ModelInfo m = deepseek_v4_flash();
    m.id = "deepseek-v4-pro";
    m.name = "DeepSeek V4 Pro";
    m.costInput = 0.435;
    m.costOutput = 0.87;
    m.costCacheRead = 0.003625;
    m.costCacheWrite = 0;
    return m;
}

std::vector<ModelInfo> build_builtin_models() { return {deepseek_v4_flash(), deepseek_v4_pro()}; }

std::vector<ModelInfo>& models()
{
    static std::vector<ModelInfo> models = build_builtin_models();
    return models;
}

}  // namespace

void register_model(const ModelInfo& model)
{
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
    auto& list = models();
    list.erase(
        std::remove_if(list.begin(), list.end(), [&](const ModelInfo& m) { return m.id == id; }),
        list.end());
}

std::optional<ModelInfo> get_model(const std::string& modelId)
{
    for (const auto& model : models())
    {
        if (model.id == modelId) return model;
    }
    return std::nullopt;
}

std::vector<std::string> get_providers()
{
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
    std::vector<ModelInfo> out;
    for (const auto& model : models())
    {
        if (model.provider == provider) out.push_back(model);
    }
    return out;
}

std::vector<ThinkingLevel> get_supported_thinking_levels(const ModelInfo& model)
{
    static const std::vector<ThinkingLevel> kAll = {
        ThinkingLevel::Off,    ThinkingLevel::Minimal, ThinkingLevel::Low,
        ThinkingLevel::Medium, ThinkingLevel::High,    ThinkingLevel::Xhigh,
    };
    if (!model.reasoning) return {ThinkingLevel::Off};

    std::vector<ThinkingLevel> supported;
    for (auto level : kAll)
    {
        const auto it = model.thinkingLevelMap.find(level);
        // 显式 null 标记为不支持；xhigh 仅当有映射值才支持；其余（未映射或映射为值）支持
        if (it != model.thinkingLevelMap.end() && !it->second.has_value()) continue;
        if (level == ThinkingLevel::Xhigh && it == model.thinkingLevelMap.end()) continue;
        supported.push_back(level);
    }
    return supported;
}

ThinkingLevel clamp_thinking_level(const ModelInfo& model, ThinkingLevel level)
{
    const auto available = get_supported_thinking_levels(model);
    if (std::find(available.begin(), available.end(), level) != available.end()) return level;

    static const std::vector<ThinkingLevel> kOrder = {
        ThinkingLevel::Off,    ThinkingLevel::Minimal, ThinkingLevel::Low,
        ThinkingLevel::Medium, ThinkingLevel::High,    ThinkingLevel::Xhigh,
    };
    auto it = std::find(kOrder.begin(), kOrder.end(), level);
    if (it == kOrder.end()) return available.empty() ? ThinkingLevel::Off : available[0];

    for (auto jt = it; jt != kOrder.end(); ++jt)
    {
        if (std::find(available.begin(), available.end(), *jt) != available.end()) return *jt;
    }
    for (auto jt = it; jt != kOrder.begin();)
    {
        --jt;
        if (std::find(available.begin(), available.end(), *jt) != available.end()) return *jt;
    }
    return available.empty() ? ThinkingLevel::Off : available[0];
}

bool models_are_equal(const ModelInfo& a, const ModelInfo& b)
{
    return a.id == b.id && a.provider == b.provider;
}

}  // namespace pi

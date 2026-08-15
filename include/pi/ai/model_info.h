#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pi/ai/types.h"

namespace pi
{

/** Mirror of pi's Model interface, trimmed to openai-completions needs. */
struct ModelInfo
{
    std::string id;
    std::string name;
    std::string api;
    std::string provider;
    std::string baseUrl;
    bool reasoning = false;
    /** Maps pi thinking levels to provider values; nullopt key marks unsupported. */
    std::map<ThinkingLevel, std::optional<std::string>> thinkingLevelMap;
    std::vector<std::string> input;  // "text" / "image"
    double costInput = 0;  // 人民币单价 ¥/M tokens
    double costOutput = 0;
    double costCacheRead = 0;
    double costCacheWrite = 0;
    int64_t contextWindow = 0;
    int64_t maxTokens = 0;
    /** Optional custom headers for this model's provider. */
    std::map<std::string, std::string> headers;

    bool supports_images() const
    {
        for (const auto& i : input)
        {
            if (i == "image") return true;
        }
        return false;
    }
};

}  // namespace pi

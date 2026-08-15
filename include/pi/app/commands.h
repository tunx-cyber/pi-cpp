#pragma once

#include <string>
#include <vector>

#include "pi/agent/types.h"

namespace pi
{

/** web_search 结果中的一个可引用来源（镜像 dsh-tool-web 的 WebSource）。 */
struct WebSearchSource
{
    std::string url;
    std::string title;
    std::string snippet;
    std::string publishedAt;
};

/** 标准化搜索结果（镜像 dsh-web 的 WebSearchResult）。 */
struct WebSearchResult
{
    std::string content;  // provider 生成的可选回答文本（DeepSeek 不返回）
    std::vector<WebSearchSource> sources;
    bool truncated = false;
    std::string error;  // 失败信息；成功时为空
};

/** web_search 的可配置参数（来自 settings.json 的 webSearch 对象）。 */
struct WebSearchConfig
{
    std::string baseUrl = "https://api.deepseek.com/anthropic/v1";
    std::string model = "deepseek-v4-flash";
    int maxTokens = 4096;
    int maxUses = 5;
    int maxResults = 8;
};

/** 编码工具（read/bash/edit/write/grep/find/ls/web_fetch/web_search），供 agent 使用。 */
std::vector<AgentTool> make_coding_tools(const std::string& cwd, const std::string& apiKey = "",
                                         const WebSearchConfig& searchConfig = {});

/** 将 DeepSeek Anthropic Messages 响应映射为标准化搜索结果（镜像 mapAnthropicResponse）。 */
WebSearchResult map_deepseek_search_response(const Json& response, int maxResults);

/** 将搜索结果格式化为面向模型的 markdown 文本（镜像 formatSearchOutput）。 */
std::string format_search_output(const WebSearchResult& result);

}  // namespace pi

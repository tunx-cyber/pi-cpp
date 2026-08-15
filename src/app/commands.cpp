#include "pi/app/commands.h"

#include <curl/curl.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "pi/harness/env.h"

namespace pi
{

// ---------- 编码工具 ----------

namespace
{

AgentTool make_tool(
    std::string name, std::string description, Json parameters,
    std::function<ToolResult(const Json&, const std::shared_ptr<std::atomic<bool>>&)> execute)
{
    AgentTool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.label = tool.name;
    tool.parameters = std::move(parameters);
    tool.execute = [execute = std::move(execute)](
                       const std::string&, const Json& args,
                       const std::shared_ptr<std::atomic<bool>>& signal,
                       const std::function<void(const ToolResult&)>&) -> ToolResult
    { return execute(args, signal); };
    return tool;
}

std::string read_file_text(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string join_lines(const std::vector<std::string>& lines, size_t maxLines)
{
    std::string out;
    for (size_t i = 0; i < lines.size() && i < maxLines; ++i)
    {
        out += lines[i] + "\n";
    }
    if (lines.size() > maxLines)
    {
        out += "[... " + std::to_string(lines.size() - maxLines) + " more lines truncated]";
    }
    return out;
}

std::string detect_workspace_root(const std::string& cwd)
{
    std::error_code ec;
    auto directory = std::filesystem::weakly_canonical(cwd, ec);
    if (ec || !std::filesystem::is_directory(directory, ec)) return cwd;

    // Running from build/ is common. Use the nearest project marker as the tool workspace,
    // while retaining cwd when the binary is launched outside a source checkout.
    while (true)
    {
        const bool has_git = std::filesystem::is_directory(directory / ".git", ec);
        const bool has_cmake = std::filesystem::is_regular_file(directory / "CMakeLists.txt", ec);
        if (has_git || has_cmake) return directory.string();
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return cwd;
}

// Resolve tool paths inside the project workspace and follow symlinks before checking containment.
// This permits build/../src while preventing escapes through ../ or symlinks.
std::optional<std::string> workspace_path(const std::string& cwd, const std::string& requested,
                                          bool allow_missing, const std::string& workspace_root)
{
    if (requested.empty()) return std::nullopt;
    std::error_code ec;
    const auto working_dir = std::filesystem::weakly_canonical(cwd, ec);
    if (ec) return std::nullopt;
    const auto root = std::filesystem::weakly_canonical(workspace_root, ec);
    if (ec) return std::nullopt;
    const auto input = std::filesystem::path(requested);
    const bool explicit_parent = !input.is_absolute() && !input.empty() &&
                                 (*input.begin() == std::filesystem::path(".."));
    const auto candidate = input.is_absolute()
                               ? input
                               : (explicit_parent ? working_dir : root) / input;
    auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
    {
        if (!allow_missing) return std::nullopt;
        const auto parent = std::filesystem::weakly_canonical(candidate.parent_path(), ec);
        if (ec) return std::nullopt;
        canonical = parent / candidate.filename();
    }
    const auto relative = std::filesystem::relative(canonical, root, ec);
    if (ec || (!relative.empty() &&
               (relative == std::filesystem::path("..") || relative.begin()->string() == "..")))
        return std::nullopt;
    return canonical.string();
}

// ---------- web fetch ----------

struct CurlFetchResult
{
    long statusCode = 0;
    std::string body;
    std::string contentType;
    std::string error;
};

struct CurlWriteContext
{
    std::string body;
    size_t maxBytes = 0;
};

size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t total = size * nmemb;
    auto* ctx = static_cast<CurlWriteContext*>(userdata);
    // 超过上限返回 0，让 curl 报 CURLE_WRITE_ERROR 并停止接收
    if (total > ctx->maxBytes || ctx->body.size() + total > ctx->maxBytes) return 0;
    ctx->body.append(ptr, total);
    return total;
}

int curl_progress_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto* signal = static_cast<const std::atomic<bool>*>(clientp);
    return signal && signal->load() ? 1 : 0;
}

/** 按 UTF-8 字符边界截断到最多 max_chars 个字符，不会切断多字节字符。 */
std::string truncate_utf8_chars(const std::string& text, size_t max_chars)
{
    if (text.size() <= max_chars) return text;
    size_t i = 0;
    size_t count = 0;
    while (i < text.size() && count < max_chars)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0)
        {
            len = 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
        }
        if (i + len > text.size()) break;
        i += len;
        ++count;
    }
    return text.substr(0, i);
}

/**
 * HTTP/HTTPS GET（libcurl）。仅允许 http/https；支持重定向、超时、大小上限、
 * abort 感知（progress callback 检查 signal）。不得抛异常——失败以 CurlFetchResult::error 返回。
 */
CurlFetchResult fetch_url(const std::string& url, int timeout_seconds, size_t max_bytes,
                          const std::shared_ptr<std::atomic<bool>>& signal)
{
    CurlFetchResult result;
    // scheme 大小写不敏感（LLM 可能输出 Https://…），其余部分按原样交给 curl
    std::string lower_prefix = url.substr(0, 8);
    std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower_prefix.rfind("http://", 0) != 0 && lower_prefix.rfind("https://", 0) != 0)
    {
        result.error = "only http/https URLs are supported";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        result.error = "curl_easy_init failed";
        return result;
    }

    CurlWriteContext ctx;
    ctx.maxBytes = max_bytes;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    // 重定向只允许 http/https（默认还含 ftp/ftps）
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_seconds) * 1000L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pi-cpp-fetch/1.0");
    // 环境代理直连本机回环（libcurl 不像 curl CLI 默认绕过代理，否则本地服务会被转发到代理）
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    // abort 感知：progress callback 返回 1 会让 curl 以 CURLE_ABORTED_BY_CALLBACK 终止
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, signal.get());

    const CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        result.statusCode = status;
        char* contentType = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK && contentType)
        {
            result.contentType = contentType;
        }
    }
    else if (res == CURLE_ABORTED_BY_CALLBACK)
    {
        result.error = "aborted";
    }
    else if (res == CURLE_WRITE_ERROR)
    {
        result.error = "response too large (max " + std::to_string(max_bytes) + " bytes)";
    }
    else
    {
        result.error = curl_easy_strerror(res);
    }

    result.body = std::move(ctx.body);
    curl_easy_cleanup(curl);
    return result;
}

// ---------- web search ----------

// DeepSeek Anthropic-compatible Messages API 常量（镜像 dsh-web-search-deepseek）
constexpr const char* kWebSearchDefaultBaseUrl = "https://api.deepseek.com/anthropic/v1";
constexpr const char* kWebSearchDefaultModel = "deepseek-v4-flash";
constexpr const char* kWebSearchApiVersion = "2023-06-01";
constexpr int kWebSearchMaxTokens = 4096;
constexpr int kWebSearchMaxUses = 5;
constexpr int kWebSearchMaxResults = 8;

/** 从 URL 提取 host（"://" 之后到第一个 / ? # 之间），作为无标题来源的兜底标签。 */
std::string hostname_of(const std::string& url)
{
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t slash = url.find_first_of("/?#", start);
    return url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
}

/**
 * DeepSeek 原生 web search（镜像 DeepSeekSearchProvider.search）：经 Anthropic 兼容
 * Messages API 调用 web_search_20250305 server tool，解析结构化结果块。
 * 不得抛异常——失败以 WebSearchResult::error 返回；abort 经 progress callback 中止。
 */
WebSearchResult deepseek_web_search(const std::string& query, const std::string& apiKey,
                                    const std::shared_ptr<std::atomic<bool>>& signal)
{
    WebSearchResult result;
    if (apiKey.empty())
    {
        result.error =
            "Missing API key. Set PI_API_KEY or DEEPSEEK_API_KEY before using web_search.";
        return result;
    }

    std::string base_url = kWebSearchDefaultBaseUrl;
    if (const char* env = std::getenv("DEEPSEEK_SEARCH_BASE_URL"); env && *env)
    {
        base_url = env;
    }
    const std::string endpoint = base_url + "/messages";

    // 请求体：镜像 DeepSeekSearchLlmRequest
    Json body;
    body["model"] = kWebSearchDefaultModel;
    body["max_tokens"] = kWebSearchMaxTokens;
    body["messages"] = Json::array({Json{
        {"role", "user"},
        {"content", Json::array({Json{{"type", "text"},
                                       {"text", "Perform a web search for the query: " + query}}})}}});
    body["tools"] = Json::array({Json{{"type", "web_search_20250305"},
                                      {"name", "web_search"},
                                      {"max_uses", kWebSearchMaxUses}}});
    const std::string body_json = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        result.error = "curl_easy_init failed";
        return result;
    }

    // 官方端点期望 x-api-key；Anthropic 兼容代理可能期望 Authorization——两者都发。
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("x-api-key: " + apiKey).c_str());
    headers = curl_slist_append(headers, ("authorization: Bearer " + apiKey).c_str());
    headers = curl_slist_append(
        headers, ("anthropic-version: " + std::string(kWebSearchApiVersion)).c_str());
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "accept: application/json");

    CurlWriteContext ctx;
    ctx.maxBytes = 5 * 1024 * 1024;  // 5MB

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_json.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, signal.get());

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK)
    {
        result.error = "aborted";
        return result;
    }
    if (res != CURLE_OK)
    {
        result.error = std::string("search request failed: ") + curl_easy_strerror(res);
        return result;
    }
    if (status < 200 || status >= 300)
    {
        std::string message = "DeepSeek search API error (HTTP " + std::to_string(status) + ")";
        try
        {
            const Json err = Json::parse(ctx.body);
            std::string detail;
            if (err.contains("error"))
            {
                if (err["error"].is_string()) detail = err["error"].get<std::string>();
                else if (err["error"].is_object() && err["error"].contains("message") &&
                         err["error"]["message"].is_string())
                    detail = err["error"]["message"].get<std::string>();
            }
            else if (err.contains("message") && err["message"].is_string())
            {
                detail = err["message"].get<std::string>();
            }
            if (!detail.empty()) message = detail;
        }
        catch (...)
        {
        }
        result.error = message;
        return result;
    }

    Json response;
    try
    {
        response = Json::parse(ctx.body);
    }
    catch (...)
    {
        result.error = "DeepSeek returned an unprocessable response body";
        return result;
    }

    return map_deepseek_search_response(response, kWebSearchMaxResults);
}

}  // namespace

/** 将 DeepSeek Anthropic Messages 响应映射为标准化搜索结果（镜像 mapAnthropicResponse）。 */
WebSearchResult map_deepseek_search_response(const Json& response, int maxResults)
{
    WebSearchResult result;
    if (!response.is_object() || !response.contains("content") || !response["content"].is_array())
    {
        result.error =
            "DeepSeek returned no web_search_tool_result blocks; the request may not have "
            "triggered native web search";
        return result;
    }

    // text 块的 citations 是 snippet 来源：url → cited_text（首次出现优先）
    std::map<std::string, std::string> snippets;
    std::vector<WebSearchSource> sources;
    std::set<std::string> seen;

    for (const auto& block : response["content"])
    {
        if (!block.is_object()) continue;
        const std::string type = block.value("type", "");
        if (type == "text")
        {
            if (block.contains("citations") && block["citations"].is_array())
            {
                for (const auto& cite : block["citations"])
                {
                    if (!cite.is_object()) continue;
                    const std::string url = cite.value("url", "");
                    const std::string cited_text = cite.value("cited_text", "");
                    if (!url.empty() && !cited_text.empty() && snippets.find(url) == snippets.end())
                    {
                        snippets[url] = cited_text;
                    }
                }
            }
        }
        else if (type == "web_search_tool_result")
        {
            if (!block.contains("content") || !block["content"].is_array()) continue;
            for (const auto& item : block["content"])
            {
                if (!item.is_object()) continue;
                if (item.value("type", "") != "web_search_result") continue;
                const std::string url = item.value("url", "");
                if (url.empty() || seen.count(url)) continue;
                seen.insert(url);
                WebSearchSource src;
                src.url = url;
                src.title = item.value("title", "");
                src.publishedAt = item.value("page_age", "");
                const auto it = snippets.find(url);
                if (it != snippets.end()) src.snippet = it->second;
                sources.push_back(std::move(src));
            }
        }
    }

    if (sources.empty())
    {
        result.error =
            "DeepSeek returned no web_search_tool_result blocks; the request may not have "
            "triggered native web search";
        return result;
    }

    if (maxResults > 0 && static_cast<int>(sources.size()) > maxResults)
    {
        sources.resize(static_cast<size_t>(maxResults));
        result.truncated = true;
    }
    result.sources = std::move(sources);
    return result;
}

/** 将搜索结果格式化为面向模型的 markdown 文本（镜像 formatSearchOutput）。 */
std::string format_search_output(const WebSearchResult& result)
{
    std::vector<std::string> parts;
    if (!result.content.empty()) parts.push_back(result.content);

    if (!result.sources.empty())
    {
        std::string lines;
        for (const auto& source : result.sources)
        {
            const std::string label = source.title.empty() ? hostname_of(source.url) : source.title;
            std::vector<std::string> meta;
            if (!source.snippet.empty()) meta.push_back(source.snippet);
            if (!source.publishedAt.empty()) meta.push_back("(" + source.publishedAt + ")");
            std::string suffix;
            for (size_t i = 0; i < meta.size(); ++i)
            {
                suffix += (i == 0 ? " — " : " ") + meta[i];
            }
            lines += "- [" + label + "](" + source.url + ")" + suffix + "\n";
        }
        parts.push_back("Sources:\n" + lines);
    }
    else if (result.content.empty())
    {
        parts.push_back("No results found.");
    }

    if (result.truncated)
    {
        parts.push_back("(Showing the first " + std::to_string(result.sources.size()) +
                        " sources. Refine the query for more.)");
    }
    parts.push_back("Cite the relevant URLs above as markdown links in your answer.");

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0) out += "\n\n";
        out += parts[i];
    }
    return out;
}

std::vector<AgentTool> make_coding_tools(const std::string& cwd, const std::string& apiKey)
{
    const std::string workspace = detect_workspace_root(cwd);
    std::vector<AgentTool> tools;

    tools.push_back(make_tool(
        "read", "Read a text file from disk. Returns the file content.",
        Json{{"type", "object"},
             {"properties", Json{{"path", Json{{"type", "string"},
                                               {"description", "Absolute or cwd-relative path"}}},
                                 {"maxLines", Json{{"type", "integer"},
                                                   {"description", "Cap on lines returned"}}}}},
             {"required", Json::array({"path"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), false, workspace);
            const std::string path = resolved.value_or("");
            ToolResult result;
            const auto info_result = [&]
            {
                struct stat st;
                if (lstat(path.c_str(), &st) != 0) return false;
                return S_ISREG(st.st_mode);
            }();
            if (!info_result)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: file not found: " + path;
                return result;
            }
            std::ifstream file(path);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(file, line))
            {
                // 长文件读取响应 abort（Ctrl+C/Esc）
                if (signal && signal->load()) break;
                lines.push_back(line);
                if (lines.size() >= 100000) break;
            }
            const int max_lines = args.contains("maxLines") ? args["maxLines"].get<int>() : 2000;
            std::string content = join_lines(lines, static_cast<size_t>(std::max(1, max_lines)));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = content.empty() ? "(empty file)" : content;
            return result;
        }));

    tools.push_back(make_tool(
        "bash", "Execute a shell command and return its output.",
        Json{{"type", "object"},
             {"properties",
              Json{{"command",
                    Json{{"type", "string"}, {"description", "Shell command to execute"}}},
                   {"cwd", Json{{"type", "string"},
                                {"description", "Working directory (defaults to session cwd)"}}}}},
             {"required", Json::array({"command"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            ToolResult result;
            PosixShell shell;
            ExecOptions options;
            const auto working_dir = workspace_path(
                cwd, args.contains("cwd") ? args["cwd"].get<std::string>() : workspace,
                false, workspace);
            if (!working_dir || !std::filesystem::is_directory(*working_dir))
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "[bash error] working directory must stay inside the workspace";
                return result;
            }
            options.cwd = *working_dir;
            options.abort = signal;
            options.timeoutSeconds = 120;
            const auto executed = shell.exec(args.value("command", ""), options);
            std::string text;
            if (!executed.ok)
            {
                text = "[bash error] " + executed.error.message;
            }
            else
            {
                text = executed.value.stdout;
                if (!executed.value.stderr.empty()) text += "\n[stderr]\n" + executed.value.stderr;
                if (executed.value.exitCode != 0)
                    text += "\n[exit code " + std::to_string(executed.value.exitCode) + "]";
            }
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no output)" : text;
            return result;
        }));

    tools.push_back(make_tool(
        "edit", "Apply a search/replace edit to a file.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}},
                   {"oldString", Json{{"type", "string"},
                                      {"description", "Text to find (must match exactly once)"}}},
                   {"newString", Json{{"type", "string"}}}}},
             {"required", Json::array({"path", "oldString", "newString"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), false, workspace);
            const std::string path = resolved.value_or("");
            ToolResult result;
            if (path.empty())
            {
                // workspace_path 对缺失文件（allow_missing=false）与越界路径都返回 nullopt，
                // 此处区分于「oldString 未命中」，给出明确报错。
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: file not found or path outside workspace: " +
                                             args.value("path", std::string(""));
                return result;
            }
            const std::string content = read_file_text(path);
            const std::string old_string = args.value("oldString", "");
            const std::string new_string = args.value("newString", "");
            size_t pos = content.find(old_string);
            if (pos == std::string::npos)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: oldString not found in " + path;
                return result;
            }
            if (content.find(old_string, pos + old_string.size()) != std::string::npos)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: oldString matches multiple times in " + path;
                return result;
            }
            std::string updated =
                content.substr(0, pos) + new_string + content.substr(pos + old_string.size());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: cannot write " + path;
                return result;
            }
            file.write(updated.data(), static_cast<std::streamsize>(updated.size()));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = "Edited " + path + " (replaced " +
                                         std::to_string(old_string.size()) + " chars with " +
                                         std::to_string(new_string.size()) + " chars)";
            return result;
        }));

    tools.push_back(make_tool(
        "write", "Create or overwrite a file with the given content.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}}, {"content", Json{{"type", "string"}}}}},
             {"required", Json::array({"path", "content"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), true, workspace);
            const std::string path = resolved.value_or("");
            if (path.empty())
            {
                ToolResult result;
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: path must stay inside the workspace";
                return result;
            }
            ToolResult result;
            const std::string dir = path.substr(0, path.find_last_of('/'));
            if (!dir.empty())
            {
                std::filesystem::create_directories(dir);
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: cannot write " + path;
                return result;
            }
            const std::string content = args.value("content", "");
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text =
                "Wrote " + std::to_string(content.size()) + " bytes to " + path;
            return result;
        }));

    tools.push_back(make_tool(
        "grep", "Search for a pattern in files, returning matching lines.",
        Json{{"type", "object"},
             {"properties", Json{{"pattern", Json{{"type", "string"}}},
                                 {"path", Json{{"type", "string"},
                                               {"description", "File or directory to search"}}},
                                 {"maxResults", Json{{"type", "integer"}}}}},
             {"required", Json::array({"pattern"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", workspace), false, workspace);
            const std::string path = resolved.value_or("");
            const std::string pattern = args.value("pattern", "");
            const int max_results =
                args.contains("maxResults") ? args["maxResults"].get<int>() : 50;
            ToolResult result;
            std::vector<std::string> matches;
            constexpr int kMaxSearchDepth = 32;
            std::function<void(const std::string&, int)> search =
                [&](const std::string& dir, int depth)
            {
                if (depth > kMaxSearchDepth) return;  // 防病态深目录栈溢出
                DIR* d = opendir(dir.c_str());
                if (!d) return;
                struct dirent* entry;
                while ((entry = readdir(d)) != nullptr &&
                       static_cast<int>(matches.size()) < max_results)
                {
                    // 目录遍历响应 abort（Ctrl+C/Esc）：返回已收集的部分结果
                    if (signal && signal->load()) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == ".git" || name == "build" ||
                        name == "node_modules")
                        continue;
                    const std::string full = dir + "/" + name;
                    struct stat st;
                    if (lstat(full.c_str(), &st) != 0) continue;
                    if (S_ISDIR(st.st_mode))
                    {
                        search(full, depth + 1);
                        continue;
                    }
                    if (!S_ISREG(st.st_mode)) continue;
                    std::ifstream file(full);
                    std::string line;
                    int line_number = 0;
                    while (std::getline(file, line) &&
                           static_cast<int>(matches.size()) < max_results)
                    {
                        if (signal && signal->load()) break;
                        ++line_number;
                        if (line.find(pattern) != std::string::npos)
                        {
                            matches.push_back(full + ":" + std::to_string(line_number) + ": " +
                                              line);
                        }
                    }
                }
                closedir(d);
            };
            struct stat st;
            if (lstat(path.c_str(), &st) != 0)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: path not found: " + path;
                return result;
            }
            if (S_ISDIR(st.st_mode))
            {
                search(path, 0);
            }
            else
            {
                std::ifstream file(path);
                std::string line;
                int line_number = 0;
                while (std::getline(file, line) && static_cast<int>(matches.size()) < max_results)
                {
                    ++line_number;
                    if (line.find(pattern) != std::string::npos)
                    {
                        matches.push_back(path + ":" + std::to_string(line_number) + ": " + line);
                    }
                }
            }
            std::string text = join_lines(matches, static_cast<size_t>(max_results));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no matches)" : text;
            return result;
        }));

    tools.push_back(make_tool(
        "find", "List files in a directory tree.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}}, {"maxResults", Json{{"type", "integer"}}}}},
             {"required", Json::array()}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", workspace), false, workspace);
            const std::string path = resolved.value_or("");
            const int max_results =
                args.contains("maxResults") ? args["maxResults"].get<int>() : 100;
            ToolResult result;
            std::vector<std::string> files;
            std::function<void(const std::string&, int)> walk =
                [&](const std::string& dir, int depth)
            {
                if (depth > 6 || static_cast<int>(files.size()) >= max_results) return;
                DIR* d = opendir(dir.c_str());
                if (!d) return;
                struct dirent* entry;
                while ((entry = readdir(d)) != nullptr &&
                       static_cast<int>(files.size()) < max_results)
                {
                    // 目录遍历响应 abort（Ctrl+C/Esc）
                    if (signal && signal->load()) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == ".git" || name == "build" ||
                        name == "node_modules")
                        continue;
                    const std::string full = dir + "/" + name;
                    struct stat st;
                    if (lstat(full.c_str(), &st) != 0) continue;
                    files.push_back(full);
                    if (S_ISDIR(st.st_mode)) walk(full, depth + 1);
                }
                closedir(d);
            };
            walk(path, 0);
            std::string text = join_lines(files, static_cast<size_t>(max_results));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no files)" : text;
            return result;
        }));

    tools.push_back(
        make_tool("ls", "List directory contents.",
                  Json{{"type", "object"},
                       {"properties", Json{{"path", Json{{"type", "string"}}}}},
                       {"required", Json::array()}},
                  [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
                  {
                      const auto resolved = workspace_path(cwd, args.value("path", workspace), false,
                                                           workspace);
                      const std::string path = resolved.value_or("");
                      ToolResult result;
                      DIR* d = opendir(path.c_str());
                      if (!d)
                      {
                          result.content.push_back(ContentBlock{});
                          result.content.back().type = BlockType::Text;
                          result.content.back().text = "Error: cannot list " + path;
                          return result;
                      }
                      std::vector<std::string> names;
                      struct dirent* entry;
                      while ((entry = readdir(d)) != nullptr)
                      {
                          const std::string name = entry->d_name;
                          if (name == "." || name == "..") continue;
                          names.push_back(name);
                      }
                      closedir(d);
                      std::sort(names.begin(), names.end());
                      std::string text = join_lines(names, names.size());
                      result.content.push_back(ContentBlock{});
                      result.content.back().type = BlockType::Text;
                      result.content.back().text = text.empty() ? "(empty directory)" : text;
                      return result;
                  }));

    tools.push_back(make_tool(
        "web_fetch", "Fetch a web page by URL and return its content as text.",
        Json{{"type", "object"},
             {"properties",
              Json{{"url", Json{{"type", "string"},
                                {"description", "Full http(s) URL to fetch"}}},
                   {"max_chars", Json{{"type", "integer"},
                                      {"description",
                                       "Maximum characters to return (UTF-8 safe, default 20000, "
                                       "max 100000)"}}},
                   {"timeout_seconds", Json{{"type", "integer"},
                                            {"description",
                                             "Timeout in seconds (default 15, max 60)"}}}}},
             {"required", Json::array({"url"})}},
        [](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            ToolResult result;
            const auto fail = [&result](const std::string& message) -> ToolResult
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "[web_fetch error] " + message;
                return result;
            };

            // LLM 可能传错参数类型（nlohmann 的 get<type>() 会抛异常），整体兜底避免整轮失败
            try
            {
                const std::string url = args.value("url", "");
                const int max_chars =
                    args.contains("max_chars") ? args["max_chars"].get<int>() : 20000;
                const int timeout_seconds =
                    args.contains("timeout_seconds") ? args["timeout_seconds"].get<int>() : 15;
                // 上限兜底：防模型传超大值导致大额下载
                const int char_cap = std::clamp(max_chars, 1, 100000);
                // 下载上限按 UTF-8 最多 4 字节/字符放大，截断时再按字符边界精确裁剪
                const size_t max_bytes = static_cast<size_t>(char_cap) * 4;
                const auto fetched =
                    fetch_url(url, std::clamp(timeout_seconds, 1, 60), max_bytes, signal);

                std::string text;
                if (!fetched.error.empty())
                {
                    text = "[web_fetch error] " + fetched.error;
                }
                else
                {
                    text = "status: " + std::to_string(fetched.statusCode);
                    if (!fetched.contentType.empty())
                    {
                        text += "\ncontent-type: " + fetched.contentType;
                    }
                    const std::string body_text =
                        truncate_utf8_chars(fetched.body, static_cast<size_t>(char_cap));
                    text += "\n\n" + body_text;
                    if (body_text.size() < fetched.body.size())
                    {
                        text += "\n[... truncated]";
                    }
                }
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = text.empty() ? "(empty response)" : text;
                return result;
            }
            catch (const std::exception& e)
            {
                return fail(std::string("invalid arguments: ") + e.what());
            }
        }));

    tools.push_back(make_tool(
        "web_search",
        "Search the web for current information. Returns an optional summary answer and a list "
        "of source URLs.",
        Json{{"type", "object"},
             {"properties",
              Json{{"query", Json{{"type", "string"}, {"description", "The search query."}}}}},
             {"required", Json::array({"query"})}},
        [apiKey](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            ToolResult result;
            const std::string query = args.value("query", "");
            if (query.empty() || query.find_first_not_of(" \t\r\n") == std::string::npos)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text =
                    "[web_search error] query must be a non-empty string";
                return result;
            }
            const auto search = deepseek_web_search(query, apiKey, signal);
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            if (!search.error.empty())
            {
                result.content.back().text = "[web_search error] " + search.error;
            }
            else
            {
                result.content.back().text = format_search_output(search);
            }
            return result;
        }));

    return tools;
}

}  // namespace pi

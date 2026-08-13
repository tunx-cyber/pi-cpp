#include "pi/ai/json_util.h"

#include <cctype>
#include <cstdio>

namespace pi
{

namespace
{

bool is_valid_json_escape(char c)
{
    return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' ||
           c == 't' || c == 'u';
}

bool is_control_character(char c) { return static_cast<unsigned char>(c) <= 0x1f; }

std::string escape_control_character(char c)
{
    switch (c)
    {
        case '\b':
            return "\\b";
        case '\f':
            return "\\f";
        case '\n':
            return "\\n";
        case '\r':
            return "\\r";
        case '\t':
            return "\\t";
        default:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            return buf;
        }
    }
}

/**
 * 返回能解析为合法 JSON 的最长前缀（parse_streaming_json 的截断回退）。
 *
 * 原实现逐字符弹出并整体重解析，对"大段未闭合字符串"（write 工具的流式参数
 * 常见形态）是 O(n²)。这里先做一次前向扫描，收集所有「可能成为完整 JSON 值
 * 末尾」的位置（跟踪字符串状态与括号深度），再从最右候选向左尝试解析。
 * 常见形态（对象/数组结尾、大段字符串）下候选数远小于 n；病态输入（顶层
 * 大量交替引号）仍可能 O(n²)，但工具参数 JSON 不会出现该形态。
 */
std::string longest_valid_json_prefix(const std::string& text)
{
    std::vector<size_t> candidates;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;  // { / [ 增，} / ] 减

    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                in_string = false;
                if (depth == 0) candidates.push_back(i);  // 顶层字符串值结束
            }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            if (depth > 0) --depth;
            if (depth == 0) candidates.push_back(i);  // 顶层结构结束
        }
        else if (depth == 0 &&
                 (std::isdigit(static_cast<unsigned char>(c)) ||
                  std::isalpha(static_cast<unsigned char>(c))))
        {
            candidates.push_back(i);  // 数字 / true / false / null 的可能末尾
        }
    }

    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
    {
        const std::string candidate = text.substr(0, *it + 1);
        try
        {
            (void)Json::parse(candidate);
            return candidate;
        }
        catch (...)
        {
        }
    }
    return "";
}

}  // namespace

std::string repair_json(const std::string& json)
{
    std::string repaired;
    repaired.reserve(json.size() + 16);
    bool in_string = false;

    for (size_t index = 0; index < json.size(); ++index)
    {
        const char ch = json[index];

        if (!in_string)
        {
            repaired += ch;
            if (ch == '"') in_string = true;
            continue;
        }

        if (ch == '"')
        {
            repaired += ch;
            in_string = false;
            continue;
        }

        if (ch == '\\')
        {
            const char next = index + 1 < json.size() ? json[index + 1] : '\0';
            if (index + 1 >= json.size())
            {
                repaired += "\\\\";
                continue;
            }
            if (next == 'u')
            {
                // 校验 \uXXXX 四字节十六进制
                bool valid = index + 6 <= json.size();
                for (size_t k = index + 2; valid && k < index + 6; ++k)
                {
                    const char h = json[k];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F')))
                        valid = false;
                }
                if (valid)
                {
                    repaired += "\\u";
                    repaired.append(json, index + 2, 4);
                    index += 5;
                    continue;
                }
            }
            if (is_valid_json_escape(next))
            {
                repaired += '\\';
                repaired += next;
                index += 1;
                continue;
            }
            repaired += "\\\\";
            continue;
        }

        repaired += is_control_character(ch) ? escape_control_character(ch) : std::string(1, ch);
    }
    return repaired;
}

Json parse_json_with_repair(const std::string& json)
{
    try
    {
        return Json::parse(json);
    }
    catch (...)
    {
        const std::string repaired = repair_json(json);
        if (repaired != json)
        {
            return Json::parse(repaired);
        }
        throw;
    }
}

Json parse_streaming_json(const std::string& partial)
{
    auto trimmed = partial;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    if (trimmed.empty()) return Json::object();

    try
    {
        return parse_json_with_repair(partial);
    }
    catch (...)
    {
        // partial-json 回退：取最长的合法 JSON 前缀（见 longest_valid_json_prefix）
        const std::string prefix = longest_valid_json_prefix(partial);
        if (!prefix.empty())
        {
            try
            {
                return Json::parse(prefix);
            }
            catch (...)
            {
            }
        }
    }
    return Json::object();
}

}  // namespace pi

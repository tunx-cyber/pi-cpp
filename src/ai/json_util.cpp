#include "pi/ai/json_util.h"

#include <cstdio>

#include <algorithm>

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
        // partial-json 回退：从尾部截断，取最长的合法 JSON 前缀
        std::string candidate = partial;
        while (!candidate.empty())
        {
            candidate.pop_back();
            try
            {
                return Json::parse(candidate);
            }
            catch (...)
            {
            }
        }
    }
    return Json::object();
}

}  // namespace pi

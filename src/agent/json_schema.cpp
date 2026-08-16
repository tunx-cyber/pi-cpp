#include "pi/agent/json_schema.h"

#include <optional>
#include <string>

#include "pi/ai/json_util.h"

namespace pi
{

namespace
{

// 轻量 JSON Schema 校验器，替代 valijson：只实现本项目工具 schema 实际用到的
// 关键字子集（type / properties / required / items / enum）。未识别的关键字按
// JSON Schema 语义视为注解忽略——既不报错、也不做约束（与 valijson 行为一致）。

std::string json_type_name(const Json& value)
{
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_number_integer()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_boolean()) return "boolean";
    if (value.is_null()) return "null";
    return "unknown";
}

bool matches_type(const Json& value, const std::string& type)
{
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return true;  // 未知 type 值：不做约束
}

// 返回校验失败原因；通过返回 nullopt。语义对齐 valijson 对 nlohmann 适配器：
// integer 仅匹配 is_number_integer()（不含 3.0 这类浮点），required 只作用于对象，
// properties 只校验「已出现」的属性。
std::optional<std::string> validate_against_schema(const Json& schema, const Json& value)
{
    if (!schema.is_object()) return std::nullopt;

    if (schema.contains("type") && schema["type"].is_string())
    {
        const std::string expected = schema["type"].get<std::string>();
        if (!matches_type(value, expected))
        {
            return "expected " + expected + ", got " + json_type_name(value);
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array())
    {
        bool matched = false;
        for (const auto& allowed : schema["enum"])
        {
            if (allowed == value)
            {
                matched = true;
                break;
            }
        }
        if (!matched) return "value not in enum";
    }

    if (value.is_object())
    {
        if (schema.contains("required") && schema["required"].is_array())
        {
            for (const auto& name : schema["required"])
            {
                if (name.is_string() && !value.contains(name.get<std::string>()))
                {
                    return "missing required property '" + name.get<std::string>() + "'";
                }
            }
        }
        if (schema.contains("properties") && schema["properties"].is_object())
        {
            for (const auto& [name, subschema] : schema["properties"].items())
            {
                if (value.contains(name))
                {
                    if (const auto error = validate_against_schema(subschema, value[name]))
                    {
                        return "property '" + name + "': " + *error;
                    }
                }
            }
        }
    }
    else if (value.is_array())
    {
        if (schema.contains("items") && schema["items"].is_object())
        {
            size_t index = 0;
            for (const auto& item : value)
            {
                if (const auto error = validate_against_schema(schema["items"], item))
                {
                    return "item " + std::to_string(index) + ": " + *error;
                }
                ++index;
            }
        }
    }

    return std::nullopt;
}

}  // namespace

std::optional<std::string> validate_tool_args(const AgentTool& tool, const Json& args)
{
    if (tool.parameters.is_null() || !tool.parameters.is_object() || tool.parameters.empty())
    {
        return std::nullopt;  // 无 schema 视为通过
    }
    if (const auto error = validate_against_schema(tool.parameters, args))
    {
        return "Invalid tool arguments for " + tool.name + ": " + *error;
    }
    return std::nullopt;
}

Json parse_tool_args(const AgentTool& tool, const std::string& rawArgs)
{
    Json args = parse_streaming_json(rawArgs);
    if (tool.prepareArguments)
    {
        try
        {
            Json prepared = tool.prepareArguments(args);
            if (prepared.is_object()) args = std::move(prepared);
        }
        catch (...)
        {
            // shim 失败保留原始解析结果
        }
    }
    return args;
}

}  // namespace pi

#include "pi/agent/json_schema.h"

#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

#include "pi/ai/json_util.h"

namespace pi
{

std::optional<std::string> validate_tool_args(const AgentTool& tool, const Json& args)
{
    if (tool.parameters.is_null() || !tool.parameters.is_object() || tool.parameters.empty())
    {
        return std::nullopt;  // 无 schema 视为通过
    }
    try
    {
        valijson::Schema schema;
        valijson::SchemaParser parser;
        valijson::adapters::NlohmannJsonAdapter schema_document(tool.parameters);
        parser.populateSchema(schema_document, schema);

        valijson::Validator validator;
        valijson::adapters::NlohmannJsonAdapter document(args);
        valijson::ValidationResults results;
        if (!validator.validate(schema, document, &results))
        {
            std::string message = "Invalid tool arguments for " + tool.name;
            valijson::ValidationResults::Error error;
            if (results.popError(error))
            {
                std::string path;
                for (const auto& segment : error.context)
                {
                    if (!path.empty()) path += "/";
                    path += segment;
                }
                message += ": " + error.description + (path.empty() ? "" : " (at /" + path + ")");
            }
            return message;
        }
    }
    catch (const std::exception& e)
    {
        return std::string("Tool schema validation error for ") + tool.name + ": " + e.what();
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

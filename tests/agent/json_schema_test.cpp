#include "pi/agent/json_schema.h"

#include <gtest/gtest.h>

#include <string>

namespace pi
{
namespace
{

// 典型编码工具 schema：type=object + properties + required（与 commands.cpp 一致）。
AgentTool object_schema_tool()
{
    AgentTool tool;
    tool.name = "edit";
    tool.parameters = Json{
        {"type", "object"},
        {"properties", Json{{"path", Json{{"type", "string"}}},
                            {"maxLines", Json{{"type", "integer"}}}}},
        {"required", Json::array({"path"})}};
    return tool;
}

TEST(JsonSchemaTest, MissingRequiredPropertyFails)
{
    const auto error = validate_tool_args(object_schema_tool(), Json::object());
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("Invalid tool arguments"), std::string::npos);
    EXPECT_NE(error->find("path"), std::string::npos);
}

TEST(JsonSchemaTest, ValidArgsPass)
{
    const auto error =
        validate_tool_args(object_schema_tool(), Json{{"path", "/tmp/x"}, {"maxLines", 10}});
    EXPECT_FALSE(error.has_value());
}

TEST(JsonSchemaTest, OptionalPropertyAbsentPasses)
{
    const auto error = validate_tool_args(object_schema_tool(), Json{{"path", "/tmp/x"}});
    EXPECT_FALSE(error.has_value());
}

TEST(JsonSchemaTest, WrongPropertyTypeFails)
{
    // maxLines 期望 integer，传字符串应失败
    const auto error =
        validate_tool_args(object_schema_tool(), Json{{"path", "/tmp/x"}, {"maxLines", "10"}});
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("maxLines"), std::string::npos);
}

TEST(JsonSchemaTest, IntegerDoesNotAcceptFloat)
{
    // 对齐 nlohmann 适配器语义：integer 只匹配 is_number_integer，3.0 是 number
    const auto error =
        validate_tool_args(object_schema_tool(), Json{{"path", "/tmp/x"}, {"maxLines", 10.0}});
    ASSERT_TRUE(error.has_value());
}

TEST(JsonSchemaTest, TopLevelNonObjectFails)
{
    const auto error = validate_tool_args(object_schema_tool(), Json("not an object"));
    ASSERT_TRUE(error.has_value());
}

TEST(JsonSchemaTest, ExtraPropertiesAllowed)
{
    const auto error =
        validate_tool_args(object_schema_tool(), Json{{"path", "/tmp/x"}, {"unknown", "extra"}});
    EXPECT_FALSE(error.has_value());
}

TEST(JsonSchemaTest, NullOrEmptySchemaPasses)
{
    AgentTool null_tool;
    null_tool.name = "probe";
    null_tool.parameters = Json(nullptr);
    EXPECT_FALSE(validate_tool_args(null_tool, Json::object()).has_value());

    AgentTool empty_tool;
    empty_tool.name = "probe";
    empty_tool.parameters = Json::object();
    EXPECT_FALSE(validate_tool_args(empty_tool, Json::object()).has_value());
}

TEST(JsonSchemaTest, EnumEnforcesMembership)
{
    AgentTool tool;
    tool.name = "color";
    tool.parameters = Json{{"type", "object"},
                           {"properties",
                            Json{{"mode", Json{{"type", "string"},
                                               {"enum", Json::array({"a", "b"})}}}}}};
    EXPECT_FALSE(validate_tool_args(tool, Json{{"mode", "a"}}).has_value());
    EXPECT_TRUE(validate_tool_args(tool, Json{{"mode", "c"}}).has_value());
}

}  // namespace
}  // namespace pi

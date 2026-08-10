#include "pi/app/commands.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

namespace pi
{
namespace
{

ToolResult run_tool(const AgentTool& tool, const Json& args)
{
    return tool.execute("test-call", args, std::make_shared<std::atomic<bool>>(false),
                        [](const ToolResult&) {});
}

TEST(CodingToolsTest, BuildDirectoryUsesProjectRootWorkspace)
{
    const auto root = std::filesystem::temp_directory_path() / "pi_coding_tools_root_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "build");
    std::ofstream(root / "CMakeLists.txt") << "project(test)\n";

    const auto tools = make_coding_tools((root / "build").string());
    const auto read_it = std::find_if(tools.begin(), tools.end(),
                                      [](const AgentTool& tool) { return tool.name == "read"; });
    ASSERT_NE(read_it, tools.end());
    const auto result = run_tool(*read_it, Json{{"path", "CMakeLists.txt"}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("project(test)"), std::string::npos);

    const auto parent_result = run_tool(*read_it, Json{{"path", "../CMakeLists.txt"}});
    ASSERT_FALSE(parent_result.content.empty());
    EXPECT_NE(parent_result.content.front().text.find("project(test)"), std::string::npos);

    std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace pi

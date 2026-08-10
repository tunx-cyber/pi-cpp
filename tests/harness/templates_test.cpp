#include <gtest/gtest.h>

#include <filesystem>

#include "pi/harness/env.h"
#include "pi/harness/prompt_templates.h"

namespace pi
{
namespace
{

TEST(ParseCommandArgsTest, SplitsOnWhitespace)
{
    const auto args = parse_command_args("foo bar baz");
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0], "foo");
    EXPECT_EQ(args[1], "bar");
    EXPECT_EQ(args[2], "baz");
}

TEST(ParseCommandArgsTest, HandlesQuotes)
{
    const auto args = parse_command_args("foo \"bar baz\" 'qux quux'");
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0], "foo");
    EXPECT_EQ(args[1], "bar baz");
    EXPECT_EQ(args[2], "qux quux");
}

TEST(ParseCommandArgsTest, EmptyInput)
{
    EXPECT_TRUE(parse_command_args("").empty());
    EXPECT_TRUE(parse_command_args("   ").empty());
}

TEST(SubstituteArgsTest, PositionalPlaceholders)
{
    EXPECT_EQ(substitute_args("echo $1 $2", {"a", "b"}), "echo a b");
    EXPECT_EQ(substitute_args("echo $1", {}), "echo ");
    EXPECT_EQ(substitute_args("$2", {"a"}), "");  // 越界为空
}

TEST(SubstituteArgsTest, AllArgsPlaceholders)
{
    EXPECT_EQ(substitute_args("run $@", {"a", "b", "c"}), "run a b c");
    EXPECT_EQ(substitute_args("run $ARGUMENTS", {"x", "y"}), "run x y");
}

TEST(SubstituteArgsTest, Slices)
{
    EXPECT_EQ(substitute_args("${@:2}", {"a", "b", "c"}), "b c");
    EXPECT_EQ(substitute_args("${@:2:1}", {"a", "b", "c"}), "b");
    EXPECT_EQ(substitute_args("${@:1:2}", {"a", "b", "c"}), "a b");
}

TEST(SubstituteArgsTest, DoesNotMatchPartialNumbers)
{
    // $1 不应匹配 $10 的前缀
    EXPECT_EQ(substitute_args("$10", {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"}),
              "j");  // $10 → 第 10 个参数
}

TEST(SubstituteArgsTest, FormatInvocation)
{
    PromptTemplate template_;
    template_.name = "test";
    template_.content = "Say $1 about $@";
    const std::string formatted = format_prompt_template_invocation(template_, {"hello", "world"});
    EXPECT_EQ(formatted, "Say hello about hello world");
}

class TemplatesLoadTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/pi_templates_test_XXXXXX";
        root_ = mkdtemp(tmpl);
        fs_ = std::make_unique<PosixFileSystem>(root_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    std::string root_;
    std::unique_ptr<PosixFileSystem> fs_;
};

TEST_F(TemplatesLoadTest, LoadsFromDirectory)
{
    fs_->write_file(root_ + "/templates/hello.md", "---\ndescription: greeting\n---\n\nHello $1");
    fs_->write_file(root_ + "/templates/other.txt", "not a template");
    const auto result = load_prompt_templates(*fs_, {root_ + "/templates"});
    ASSERT_EQ(result.promptTemplates.size(), 1u);
    EXPECT_EQ(result.promptTemplates[0].name, "hello");
    EXPECT_EQ(result.promptTemplates[0].description, "greeting");
    EXPECT_EQ(result.promptTemplates[0].content, "Hello $1");
}

TEST_F(TemplatesLoadTest, LoadsSingleFile)
{
    fs_->write_file(root_ + "/single.md", "---\n---\n\nBody here");
    const auto result = load_prompt_templates(*fs_, {root_ + "/single.md"});
    ASSERT_EQ(result.promptTemplates.size(), 1u);
    EXPECT_EQ(result.promptTemplates[0].name, "single");
    EXPECT_EQ(result.promptTemplates[0].content, "Body here");
}

TEST_F(TemplatesLoadTest, DescriptionFallsBackToFirstLine)
{
    fs_->write_file(root_ + "/t.md", "First line is the description\nSecond line");
    const auto result = load_prompt_templates(*fs_, {root_ + "/t.md"});
    ASSERT_EQ(result.promptTemplates.size(), 1u);
    EXPECT_EQ(result.promptTemplates[0].description, "First line is the description");
}

}  // namespace
}  // namespace pi

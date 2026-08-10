#include "pi/harness/skills.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "pi/harness/env.h"

namespace pi
{
namespace
{

class SkillsTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/pi_skills_test_XXXXXX";
        root_ = mkdtemp(tmpl);
        fs_ = std::make_unique<PosixFileSystem>(root_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void write(const std::string& rel, const std::string& content)
    {
        const auto result = fs_->write_file(root_ + "/" + rel, content);
        ASSERT_TRUE(result.ok) << result.error.message;
    }

    std::string root_;
    std::unique_ptr<PosixFileSystem> fs_;
};

TEST_F(SkillsTest, LoadsNestedSkillMd)
{
    write("skills/test-skill/SKILL.md",
          "---\nname: test-skill\ndescription: A test skill\n---\n\nDo the thing.");
    const auto result = load_skills(*fs_, {root_ + "/skills"});
    ASSERT_EQ(result.skills.size(), 1u);
    EXPECT_EQ(result.skills[0].name, "test-skill");
    EXPECT_EQ(result.skills[0].description, "A test skill");
    EXPECT_NE(result.skills[0].content.find("Do the thing."), std::string::npos);
}

TEST_F(SkillsTest, NameFallsBackToParentDir)
{
    write("skills/fallback-name/SKILL.md", "---\ndescription: desc here\n---\n\nbody");
    const auto result = load_skills(*fs_, {root_ + "/skills"});
    ASSERT_EQ(result.skills.size(), 1u);
    EXPECT_EQ(result.skills[0].name, "fallback-name");
}

TEST_F(SkillsTest, SkipsSkillsWithoutDescription)
{
    write("skills/nodesc/SKILL.md", "---\nname: nodesc\n---\n\nbody");
    const auto result = load_skills(*fs_, {root_ + "/skills"});
    EXPECT_TRUE(result.skills.empty());
    // 产生 invalid_metadata 诊断
    bool saw_diagnostic = false;
    for (const auto& diagnostic : result.diagnostics)
    {
        if (diagnostic.code == "invalid_metadata") saw_diagnostic = true;
    }
    EXPECT_TRUE(saw_diagnostic);
}

TEST_F(SkillsTest, HonorsDisableModelInvocation)
{
    write("skills/hidden/SKILL.md",
          "---\nname: hidden\ndescription: hidden skill\ndisable-model-invocation: "
          "true\n---\n\nbody");
    const auto result = load_skills(*fs_, {root_ + "/skills"});
    ASSERT_EQ(result.skills.size(), 1u);
    EXPECT_TRUE(result.skills[0].disableModelInvocation);

    const std::string formatted = format_skills_for_system_prompt(result.skills);
    EXPECT_TRUE(formatted.empty());
}

TEST_F(SkillsTest, LoadsRootLevelMdFiles)
{
    write("skills/root-skill.md", "---\ndescription: root level\n---\n\nroot body");
    const auto result = load_skills(*fs_, {root_ + "/skills"});
    ASSERT_EQ(result.skills.size(), 1u);
    // 无 frontmatter name 时回退到父目录名（与 pi 行为一致）
    EXPECT_EQ(result.skills[0].name, "skills");
}

TEST_F(SkillsTest, FormatsSystemPromptBlock)
{
    Skill skill;
    skill.name = "my-skill";
    skill.description = "does <things> & more";
    skill.filePath = "/tmp/skills/my-skill/SKILL.md";
    const std::string formatted = format_skills_for_system_prompt({skill});
    EXPECT_NE(formatted.find("<available_skills>"), std::string::npos);
    EXPECT_NE(formatted.find("<name>my-skill</name>"), std::string::npos);
    // XML 转义
    EXPECT_NE(formatted.find("does &lt;things&gt; &amp; more"), std::string::npos);
    EXPECT_NE(formatted.find("<location>/tmp/skills/my-skill/SKILL.md</location>"),
              std::string::npos);
}

TEST_F(SkillsTest, MissingDirSkippedSilently)
{
    const auto result = load_skills(*fs_, {root_ + "/does-not-exist"});
    EXPECT_TRUE(result.skills.empty());
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(SkillsTest, FormatSkillInvocation)
{
    Skill skill;
    skill.name = "my-skill";
    skill.content = "instructions";
    skill.filePath = "/tmp/skills/my-skill/SKILL.md";
    const std::string invocation = format_skill_invocation(skill, "extra");
    EXPECT_NE(
        invocation.find("<skill name=\"my-skill\" location=\"/tmp/skills/my-skill/SKILL.md\">"),
        std::string::npos);
    EXPECT_NE(invocation.find("instructions"), std::string::npos);
    EXPECT_NE(invocation.find("extra"), std::string::npos);
}

}  // namespace
}  // namespace pi

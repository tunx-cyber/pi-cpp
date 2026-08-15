#include "pi/app/settings.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

namespace pi
{
namespace
{

class SettingsTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        char temporary[] = "/tmp/pi_settings_test_XXXXXX";
        ASSERT_NE(mkdtemp(temporary), nullptr);
        home_ = temporary;
        const char* old_home = std::getenv("HOME");
        if (old_home) old_home_ = old_home;
        ASSERT_EQ(setenv("HOME", home_.c_str(), 1), 0);
    }

    void TearDown() override
    {
        if (old_home_.empty())
            unsetenv("HOME");
        else
            setenv("HOME", old_home_.c_str(), 1);
        std::filesystem::remove_all(home_);
    }

    std::string home_;
    std::string old_home_;
};

TEST_F(SettingsTest, PersistsCwdOverrides)
{
    Settings settings;
    settings.set_override("/workspace", "deepseek-v4-pro", ThinkingLevel::High);

    const auto override = Settings::load().override_for("/workspace");
    ASSERT_TRUE(override.has_value());
    ASSERT_TRUE(override->model.has_value());
    ASSERT_TRUE(override->thinking.has_value());
    EXPECT_EQ(*override->model, "deepseek-v4-pro");
    EXPECT_EQ(*override->thinking, ThinkingLevel::High);

    const auto settings_path = Settings::expand_home("~/.pi-cpp/settings.json");
    EXPECT_EQ(std::filesystem::status(settings_path).permissions() & std::filesystem::perms::group_read,
              std::filesystem::perms::none);
}

TEST_F(SettingsTest, PersistsPricingOverride)
{
    Settings settings;
    settings.pricing.input = 1.5;
    settings.pricing.output = 3.0;
    settings.pricing.cacheRead = 0.1;
    settings.pricing.cacheWrite = 0.2;
    settings.save();

    const auto loaded = Settings::load();
    ASSERT_TRUE(loaded.pricing.input.has_value());
    ASSERT_TRUE(loaded.pricing.output.has_value());
    ASSERT_TRUE(loaded.pricing.cacheRead.has_value());
    ASSERT_TRUE(loaded.pricing.cacheWrite.has_value());
    EXPECT_DOUBLE_EQ(*loaded.pricing.input, 1.5);
    EXPECT_DOUBLE_EQ(*loaded.pricing.output, 3.0);
    EXPECT_DOUBLE_EQ(*loaded.pricing.cacheRead, 0.1);
    EXPECT_DOUBLE_EQ(*loaded.pricing.cacheWrite, 0.2);
}

TEST_F(SettingsTest, PersistsCustomModelsAndExtendedConfig)
{
    Settings settings;
    settings.sessionsRoot = "~/.custom-sessions";
    settings.systemPrompt = "custom system prompt";
    ModelInfo custom;
    custom.id = "qwen2.5-coder";
    custom.name = "Qwen 2.5 Coder";
    custom.baseUrl = "http://localhost:11434/v1";
    custom.provider = "custom";
    custom.contextWindow = 32768;
    custom.maxTokens = 8192;
    custom.costInput = 0.5;
    custom.costOutput = 1.0;
    settings.models.push_back(custom);
    settings.compaction.enabled = false;
    settings.compaction.reserveTokens = 8000;
    settings.compaction.keepRecentTokens = 10000;
    settings.webSearch.model = "deepseek-v4-pro";
    settings.webSearch.maxTokens = 2048;
    settings.save();

    const auto loaded = Settings::load();
    EXPECT_EQ(loaded.sessionsRoot, "~/.custom-sessions");
    EXPECT_EQ(loaded.systemPrompt, "custom system prompt");
    ASSERT_EQ(loaded.models.size(), 1u);
    EXPECT_EQ(loaded.models[0].id, "qwen2.5-coder");
    EXPECT_EQ(loaded.models[0].name, "Qwen 2.5 Coder");
    EXPECT_EQ(loaded.models[0].baseUrl, "http://localhost:11434/v1");
    EXPECT_EQ(loaded.models[0].provider, "custom");
    EXPECT_EQ(loaded.models[0].contextWindow, 32768);
    EXPECT_EQ(loaded.models[0].maxTokens, 8192);
    EXPECT_DOUBLE_EQ(loaded.models[0].costInput, 0.5);
    EXPECT_DOUBLE_EQ(loaded.models[0].costOutput, 1.0);
    EXPECT_FALSE(loaded.compaction.enabled);
    EXPECT_EQ(loaded.compaction.reserveTokens, 8000);
    EXPECT_EQ(loaded.compaction.keepRecentTokens, 10000);
    EXPECT_EQ(loaded.webSearch.model, "deepseek-v4-pro");
    EXPECT_EQ(loaded.webSearch.maxTokens, 2048);
}

}  // namespace
}  // namespace pi

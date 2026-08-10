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

}  // namespace
}  // namespace pi

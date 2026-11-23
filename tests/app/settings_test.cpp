#include "pi/app/settings.h"

#include <gtest/gtest.h>

#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace pi
{
namespace
{

/** 临时清除/设置环境变量（析构时恢复），避免宿主环境干扰 Settings::load()。 */
class ScopedEnvVar
{
   public:
    explicit ScopedEnvVar(const char* name, const char* value = nullptr) : name_(name)
    {
        if (const char* old = std::getenv(name)) old_ = std::string(old);
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }
    ~ScopedEnvVar()
    {
        if (old_)
            setenv(name_.c_str(), old_->c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

   private:
    std::string name_;
    std::optional<std::string> old_;
};

/** 临时切换 cwd（确保不会读到仓库根目录的 .env）。 */
class ScopedCurrentPath
{
   public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : old_(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }
    ~ScopedCurrentPath()
    {
        std::error_code ec;
        std::filesystem::current_path(old_, ec);
    }

   private:
    std::filesystem::path old_;
};

/** 隔离 HOME 与 cwd：既不会读到真实 ~/.pi-cpp，也不会读到仓库 .env。 */
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

TEST_F(SettingsTest, DefaultsWithNoEnv)
{
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar openai_key("OPENAI_API_KEY");
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedEnvVar model("PI_MODEL");
    ScopedEnvVar thinking("PI_THINKING");
    ScopedEnvVar sessions_root("PI_SESSIONS_ROOT");
    ScopedEnvVar system_prompt("PI_SYSTEM_PROMPT");
    ScopedEnvVar theme("PI_THEME");
    ScopedCurrentPath cwd(home_);

    const auto settings = Settings::load();
    EXPECT_TRUE(settings.apiKey.empty());
    EXPECT_EQ(settings.baseUrl, "https://api.deepseek.com/v1");
    EXPECT_TRUE(settings.model.empty());  // 不写死默认模型
    EXPECT_EQ(settings.thinking, ThinkingLevel::Off);
    EXPECT_EQ(settings.sessionsRoot, "~/.pi-cpp/agent/sessions");
    EXPECT_EQ(settings.theme, "default");
    EXPECT_FALSE(settings.systemPrompt.empty());
}

TEST_F(SettingsTest, ReadsApiKeyWithPrecedence)
{
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar openai_key("OPENAI_API_KEY");
    ScopedEnvVar deepseek_key("DEEPSEEK_API_KEY");
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedCurrentPath cwd(home_);

    // DEEPSEEK_API_KEY 不再是受支持的变量
    setenv("DEEPSEEK_API_KEY", "deepseek-key", 1);
    EXPECT_TRUE(Settings::load().apiKey.empty());

    setenv("OPENAI_API_KEY", "openai-key", 1);
    EXPECT_EQ(Settings::load().apiKey, "openai-key");

    setenv("PI_API_KEY", "pi-key", 1);
    EXPECT_EQ(Settings::load().apiKey, "pi-key");
}

TEST_F(SettingsTest, NormalizesBareBaseUrl)
{
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedCurrentPath cwd(home_);

    setenv("PI_BASE_URL", "https://api.example.com", 1);
    EXPECT_EQ(Settings::load().baseUrl, "https://api.example.com/v1");

    setenv("PI_BASE_URL", "https://api.example.com/v1/", 1);
    EXPECT_EQ(Settings::load().baseUrl, "https://api.example.com/v1");

    // 带路径的端点原样保留（自建网关的 /openai/v1、Anthropic 的 /anthropic/v1）
    setenv("PI_BASE_URL", "https://gateway.example.com/openai/v1", 1);
    EXPECT_EQ(Settings::load().baseUrl, "https://gateway.example.com/openai/v1");
}

TEST_F(SettingsTest, ReadsModelThinkingThemeAndPaths)
{
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedEnvVar model("PI_MODEL", "mimo-v2.6-flash");
    ScopedEnvVar thinking("PI_THINKING", "high");
    ScopedEnvVar sessions_root("PI_SESSIONS_ROOT", "/tmp/pi-test-sessions");
    ScopedEnvVar system_prompt("PI_SYSTEM_PROMPT", "custom system prompt");
    ScopedEnvVar theme("PI_THEME", "mono");
    ScopedCurrentPath cwd(home_);

    const auto settings = Settings::load();
    EXPECT_EQ(settings.model, "mimo-v2.6-flash");
    EXPECT_EQ(settings.thinking, ThinkingLevel::High);
    EXPECT_EQ(settings.sessionsRoot, "/tmp/pi-test-sessions");
    EXPECT_EQ(settings.systemPrompt, "custom system prompt");
    EXPECT_EQ(settings.theme, "mono");
}

TEST_F(SettingsTest, ReadsProjectDotEnv)
{
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedEnvVar model("PI_MODEL");
    ScopedCurrentPath cwd(home_);

    {
        std::ofstream file(std::filesystem::path(home_) / ".env");
        file << "PI_API_KEY=from-dotenv\nPI_BASE_URL=https://api.example.com\nPI_MODEL=model-x\n";
    }

    const auto settings = Settings::load();
    EXPECT_EQ(settings.apiKey, "from-dotenv");
    EXPECT_EQ(settings.baseUrl, "https://api.example.com/v1");
    EXPECT_EQ(settings.model, "model-x");
}

TEST_F(SettingsTest, ProcessEnvOverridesDotEnv)
{
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar model("PI_MODEL", "from-env");
    ScopedCurrentPath cwd(home_);

    {
        std::ofstream file(std::filesystem::path(home_) / ".env");
        file << "PI_MODEL=from-dotenv\n";
    }

    EXPECT_EQ(Settings::load().model, "from-env");
}

TEST_F(SettingsTest, IgnoresSettingsJson)
{
    // settings.json 不再被读取：旧文件里的 model/theme 等键不影响运行配置
    ScopedEnvVar api_key("PI_API_KEY");
    ScopedEnvVar base_url("PI_BASE_URL");
    ScopedEnvVar model("PI_MODEL");
    ScopedEnvVar theme("PI_THEME");
    ScopedCurrentPath cwd(home_);

    const auto dir = std::filesystem::path(home_) / ".pi-cpp";
    std::filesystem::create_directories(dir);
    {
        std::ofstream file(dir / "settings.json");
        file << R"({"model":"file-model","theme":"mono","sessionsRoot":"/tmp/file-sessions"})";
    }

    const auto settings = Settings::load();
    EXPECT_TRUE(settings.model.empty());
    EXPECT_EQ(settings.theme, "default");
    EXPECT_EQ(settings.sessionsRoot, "~/.pi-cpp/agent/sessions");
}

}  // namespace
}  // namespace pi

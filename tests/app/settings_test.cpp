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

/** 临时清除环境变量（析构时恢复），避免宿主环境干扰 Settings::load()。 */
class ScopedEnvVar
{
   public:
    explicit ScopedEnvVar(const char* name) : name_(name)
    {
        if (const char* value = std::getenv(name)) old_ = std::string(value);
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

/** 临时切换 cwd（析构时恢复），确保不会读到仓库根目录的 .env。 */
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
    EXPECT_EQ(
        std::filesystem::status(settings_path).permissions() & std::filesystem::perms::group_read,
        std::filesystem::perms::none);
}

TEST_F(SettingsTest, DoesNotPersistApiCredentials)
{
    // 预置旧版本遗留的凭据键，save() 后应被移除
    const auto settings_path = Settings::expand_home("~/.pi-cpp/settings.json");
    std::filesystem::create_directories(std::filesystem::path(settings_path).parent_path());
    {
        std::ofstream file(settings_path);
        file << R"({"apiKey":"old-key","baseUrl":"https://old.invalid","model":"old-model"})";
    }

    Settings settings;
    settings.apiKey = "secret";
    settings.baseUrl = "https://example.invalid";
    settings.model = "file-model";
    settings.save();

    std::ifstream file(settings_path);
    ASSERT_TRUE(file.is_open());
    const Json json = Json::parse(file);
    EXPECT_FALSE(json.contains("apiKey"));
    EXPECT_FALSE(json.contains("baseUrl"));
    EXPECT_EQ(json.value("model", std::string("")), "file-model");
}

TEST_F(SettingsTest, IgnoresApiCredentialsFromSettingsFile)
{
    // 清空相关 env 并离开仓库目录，确保只验证 settings.json 中的键被忽略
    ScopedEnvVar pi_key("PI_API_KEY");
    ScopedEnvVar deepseek_key("DEEPSEEK_API_KEY");
    ScopedEnvVar openai_key("OPENAI_API_KEY");
    ScopedEnvVar pi_base("PI_BASE_URL");
    ScopedEnvVar pi_model("PI_MODEL");
    ScopedCurrentPath cwd(home_);

    const auto dir = std::filesystem::path(home_) / ".pi-cpp";
    std::filesystem::create_directories(dir);
    {
        std::ofstream file(dir / "settings.json");
        file << R"({"apiKey":"file-key","baseUrl":"https://file.invalid","model":"file-model"})";
    }

    const auto loaded = Settings::load();
    EXPECT_TRUE(loaded.apiKey.empty());
    // 默认端点按 OpenAI 兼容惯例规范化：裸域名补 /v1
    EXPECT_EQ(loaded.baseUrl, "https://api.deepseek.com/v1");
    EXPECT_EQ(loaded.model, "file-model");  // 其他键仍从 settings.json 读取
}

TEST_F(SettingsTest, FallsBackToDeepSeekApiKey)
{
    ScopedEnvVar pi_key("PI_API_KEY");
    ScopedEnvVar deepseek_key("DEEPSEEK_API_KEY");
    ScopedEnvVar openai_key("OPENAI_API_KEY");
    ScopedEnvVar pi_base("PI_BASE_URL");
    ScopedCurrentPath cwd(home_);

    setenv("DEEPSEEK_API_KEY", "deepseek-key", 1);
    EXPECT_EQ(Settings::load().apiKey, "deepseek-key");

    // PI_API_KEY 优先于 DEEPSEEK_API_KEY
    setenv("PI_API_KEY", "pi-key", 1);
    EXPECT_EQ(Settings::load().apiKey, "pi-key");
}

TEST_F(SettingsTest, NormalizesBareBaseUrl)
{
    ScopedEnvVar pi_base("PI_BASE_URL");
    ScopedEnvVar pi_key("PI_API_KEY");
    ScopedCurrentPath cwd(home_);

    const auto with_bare_host = []
    {
        setenv("PI_BASE_URL", "https://api.example.com", 1);
        return Settings::load().baseUrl;
    }();
    EXPECT_EQ(with_bare_host, "https://api.example.com/v1");

    const auto with_trailing_slash = []
    {
        setenv("PI_BASE_URL", "https://api.example.com/v1/", 1);
        return Settings::load().baseUrl;
    }();
    EXPECT_EQ(with_trailing_slash, "https://api.example.com/v1");

    // 带路径的端点原样保留（例如自建网关的 /openai/v1、Anthropic 的 /anthropic/v1）
    const auto with_path = []
    {
        setenv("PI_BASE_URL", "https://gateway.example.com/openai/v1", 1);
        return Settings::load().baseUrl;
    }();
    EXPECT_EQ(with_path, "https://gateway.example.com/openai/v1");
}

TEST_F(SettingsTest, DefaultsToNoModel)
{
    // 不写死任何默认模型：没有 settings.json / PI_MODEL 时 model 为空，
    // 由运行时 GET /models 返回的第一个模型确定。
    ScopedEnvVar pi_model("PI_MODEL");
    ScopedCurrentPath cwd(home_);

    const auto loaded = Settings::load();
    EXPECT_TRUE(loaded.model.empty());
}

TEST_F(SettingsTest, DoesNotPersistUnresolvedModel)
{
    // 自动探测出的模型不应被固化成 settings.json 的默认值
    Settings settings;
    ASSERT_TRUE(settings.model.empty());
    settings.save();

    const auto settings_path = Settings::expand_home("~/.pi-cpp/settings.json");
    std::ifstream file(settings_path);
    ASSERT_TRUE(file.is_open());
    const Json json = Json::parse(file);
    EXPECT_FALSE(json.contains("model"));
}

TEST_F(SettingsTest, CustomModelsFallBackToEnvBaseUrl)
{
    ScopedEnvVar pi_key("PI_API_KEY");
    ScopedEnvVar deepseek_key("DEEPSEEK_API_KEY");
    ScopedEnvVar openai_key("OPENAI_API_KEY");
    ScopedEnvVar pi_base("PI_BASE_URL");
    ScopedEnvVar pi_model("PI_MODEL");
    ScopedCurrentPath cwd(home_);
    ASSERT_EQ(setenv("PI_BASE_URL", "https://proxy.example/v1", 1), 0);

    const auto dir = std::filesystem::path(home_) / ".pi-cpp";
    std::filesystem::create_directories(dir);
    {
        std::ofstream file(dir / "settings.json");
        // 模型未显式给 baseUrl → 应回退到 env 的 PI_BASE_URL（而不是代码默认值）
        file << R"({"baseUrl":"https://file.invalid","models":[{"id":"m1"}]})";
    }

    const auto loaded = Settings::load();
    EXPECT_EQ(loaded.baseUrl, "https://proxy.example/v1");
    ASSERT_EQ(loaded.models.size(), 1U);
    EXPECT_EQ(loaded.models[0].baseUrl, "https://proxy.example/v1");
}

TEST_F(SettingsTest, PersistsCustomModelsAndExtendedConfig)
{
    Settings settings;
    settings.sessionsRoot = "~/.custom-sessions";
    settings.theme = "mono";
    settings.systemPrompt = "custom system prompt";
    ModelInfo custom;
    custom.id = "qwen2.5-coder";
    custom.name = "Qwen 2.5 Coder";
    custom.baseUrl = "http://localhost:11434/v1";
    custom.provider = "custom";
    custom.contextWindow = 32768;
    custom.maxTokens = 8192;
    settings.models.push_back(custom);
    settings.compaction.reserveTokens = 8000;
    settings.compaction.keepRecentTokens = 10000;
    settings.webSearch.model = "deepseek-v4-pro";
    settings.webSearch.maxTokens = 2048;
    settings.save();

    const auto loaded = Settings::load();
    EXPECT_EQ(loaded.sessionsRoot, "~/.custom-sessions");
    EXPECT_EQ(loaded.theme, "mono");
    EXPECT_EQ(loaded.systemPrompt, "custom system prompt");
    ASSERT_EQ(loaded.models.size(), 1u);
    EXPECT_EQ(loaded.models[0].id, "qwen2.5-coder");
    EXPECT_EQ(loaded.models[0].name, "Qwen 2.5 Coder");
    EXPECT_EQ(loaded.models[0].baseUrl, "http://localhost:11434/v1");
    EXPECT_EQ(loaded.models[0].provider, "custom");
    EXPECT_EQ(loaded.models[0].contextWindow, 32768);
    EXPECT_EQ(loaded.models[0].maxTokens, 8192);
    EXPECT_EQ(loaded.compaction.reserveTokens, 8000);
    EXPECT_EQ(loaded.compaction.keepRecentTokens, 10000);
    EXPECT_EQ(loaded.webSearch.model, "deepseek-v4-pro");
    EXPECT_EQ(loaded.webSearch.maxTokens, 2048);
}

}  // namespace
}  // namespace pi

#include "pi/app/agent_session.h"

#include <gtest/gtest.h>

#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "pi/ai/model_registry.h"
#include "test_utils/fake_sse_server.h"

namespace pi
{
namespace
{

/** 临时清除环境变量（析构时恢复）。 */
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

/** 临时切换 cwd（避免读到仓库根目录的 .env）。 */
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

/** 临时 HOME（设置写入 ~/.pi-cpp/settings.json，测试不能污染真实配置）。 */
class ScopedHome
{
   public:
    ScopedHome()
    {
        char path[] = "/tmp/pi_agent_session_home_XXXXXX";
        if (mkdtemp(path) == nullptr) return;
        root_ = path;
        if (const char* old = std::getenv("HOME")) old_ = std::string(old);
        setenv("HOME", root_.c_str(), 1);
    }
    const std::string& path() const { return root_; }
    ~ScopedHome()
    {
        if (old_)
            setenv("HOME", old_->c_str(), 1);
        else
            unsetenv("HOME");
        if (!root_.empty()) std::filesystem::remove_all(root_);
    }

   private:
    std::string root_;
    std::optional<std::string> old_;
};

TEST(AgentSessionModelsTest, ListsModelsFromOpenAiCompatibleEndpoint)
{
    pi_test::FakeSseServer server;
    server.set_handler(
        [](const std::string&, const std::map<std::string, std::string>&)
        { return std::string(R"({"object":"list","data":[{"id":"model-a"},{"id":"model-b"}]})"); });

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd");

    const auto list = session.list_models();
    ASSERT_TRUE(list.error.empty()) << list.error;
    ASSERT_EQ(list.models.size(), 2U);
    EXPECT_EQ(list.models[0].id, "model-a");
    EXPECT_EQ(list.models[1].id, "model-b");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].path, "/v1/models");
}

TEST(AgentSessionModelsTest, ReportsEndpointFailureWithoutThrowing)
{
    pi_test::FakeSseServer server;
    server.set_handler([](const std::string&, const std::map<std::string, std::string>&)
                       { return std::string(R"({"error":{"message":"service unavailable"}})"); });
    server.set_status(503);

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd");

    const auto list = session.list_models();
    EXPECT_TRUE(list.models.empty());
    EXPECT_EQ(list.error, "service unavailable");
}

TEST(AgentSessionModelsTest, ResolvesDefaultModelFromEndpoint)
{
    pi_test::FakeSseServer server;
    server.set_handler(
        [](const std::string&, const std::map<std::string, std::string>&)
        { return std::string(R"({"object":"list","data":[{"id":"model-a"},{"id":"model-b"}]})"); });

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd");
    session.new_session();

    // 未配置模型 → 默认模型 = GET /models 返回的第一个模型
    std::string error;
    ASSERT_TRUE(session.ensure_model(&error)) << error;
    EXPECT_EQ(session.model().id, "model-a");

    // 已有模型后不再请求端点
    ASSERT_TRUE(session.ensure_model(&error)) << error;
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(AgentSessionModelsTest, EnsureModelReportsEndpointFailure)
{
    pi_test::FakeSseServer server;
    server.set_handler([](const std::string&, const std::map<std::string, std::string>&)
                       { return std::string(R"({"error":{"message":"service unavailable"}})"); });
    server.set_status(503);

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd");
    session.new_session();

    std::string error;
    EXPECT_FALSE(session.ensure_model(&error));
    EXPECT_NE(error.find("service unavailable"), std::string::npos) << error;
    EXPECT_TRUE(session.model().id.empty());
}

TEST(AgentSessionModelsTest, RemoteModelInheritsFamilyCapabilities)
{
    // 端点只返回 id：默认模型仍应带上 reasoning/thinking 映射，而不是被当成纯文本模型
    pi_test::FakeSseServer server;
    server.set_handler(
        [](const std::string&, const std::map<std::string, std::string>&)
        { return std::string(R"({"object":"list","data":[{"id":"deepseek-flash"}]})"); });

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-family");
    session.new_session();

    std::string error;
    ASSERT_TRUE(session.ensure_model(&error)) << error;
    EXPECT_EQ(session.model().id, "deepseek-flash");
    // 只继承 reasoning/thinking 映射元数据；是否"支持"由真实请求决定
    EXPECT_TRUE(session.model().reasoning);
    EXPECT_FALSE(session.model().thinkingLevelMap.empty());
}

TEST(AgentSessionModelsTest, InjectsSkillsIntoSystemPrompt)
{
    // ~/.pi-cpp/skills 下的 SKILL.md 必须进入 system prompt，模型才"看得到"技能
    ScopedHome home;
    const auto skill_dir = std::filesystem::path(home.path()) / ".pi-cpp" / "skills" / "demo";
    std::filesystem::create_directories(skill_dir);
    {
        std::ofstream file(skill_dir / "SKILL.md");
        file << "---\nname: demo\ndescription: Demo skill\n---\n\nDo the demo.\n";
    }

    Settings settings;
    settings.baseUrl = "http://127.0.0.1:1";  // 已有模型，不会请求端点
    settings.model = "deepseek-v4-pro";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-skills");
    session.new_session();

    const std::string prompt = session.system_prompt();
    EXPECT_NE(prompt.find("<available_skills>"), std::string::npos);
    EXPECT_NE(prompt.find("<name>demo</name>"), std::string::npos);
    EXPECT_NE(prompt.find("Demo skill"), std::string::npos);
}

TEST(AgentSessionModelsTest, NewSessionStartsFreshTranscript)
{
    // /clear 走 new_session：必须换新会话文件并清空 transcript
    ScopedHome home;
    Settings settings;
    settings.baseUrl = "http://127.0.0.1:1";
    settings.model = "deepseek-v4-pro";
    settings.sessionsRoot = "/tmp/pi-agent-session-test-sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-clear");
    session.new_session();
    const std::string first = session.current_session_id();
    EXPECT_FALSE(first.empty());

    session.new_session();
    EXPECT_NE(session.current_session_id(), first);
    EXPECT_TRUE(session.messages().empty());
}

TEST(AgentSessionModelsTest, AutoDetectedModelIsNotPersisted)
{
    // ensure_model 的探测结果只在进程内有效：后续 /thinking 写配置时不能把它落盘
    ScopedHome home;
    ScopedEnvVar pi_model("PI_MODEL");
    ScopedCurrentPath cwd(home.path());

    pi_test::FakeSseServer server;
    server.set_handler([](const std::string&, const std::map<std::string, std::string>&)
                       { return std::string(R"({"object":"list","data":[{"id":"model-a"}]})"); });

    Settings settings;
    settings.baseUrl = server.base_url();
    settings.apiKey = "test-key";
    settings.sessionsRoot = home.path() + "/sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-autopersist");
    session.new_session();

    std::string error;
    ASSERT_TRUE(session.ensure_model(&error)) << error;
    session.set_thinking(ThinkingLevel::High);

    const auto loaded = Settings::load();
    EXPECT_TRUE(loaded.model.empty());
    const auto override = loaded.override_for("/tmp/pi-agent-session-test-cwd-autopersist");
    if (override) EXPECT_FALSE(override->model.has_value());
}

TEST(AgentSessionModelsTest, ThinkingLevelIsNotClampedByModel)
{
    // 不再根据模型能力过滤 thinking：用户选什么就用什么，真实请求失败时报错
    ScopedHome home;
    Settings settings;
    settings.baseUrl = "http://127.0.0.1:1";
    settings.model = "deepseek-v4-pro";
    settings.sessionsRoot = home.path() + "/sessions";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-thinking-noclamp");
    session.new_session();

    session.set_thinking(ThinkingLevel::Medium);
    EXPECT_EQ(session.thinking_level(), ThinkingLevel::Medium);

    // 切换模型也不回退/校准
    session.set_model("deepseek-v4-flash");
    EXPECT_EQ(session.thinking_level(), ThinkingLevel::Medium);
}

TEST(AgentSessionModelsTest, KeepsCwdModelOverrideWhenChangingThinking)
{
    // 先 /model 再 /thinking 不应丢掉按 cwd 保存的模型选择
    ScopedHome home;
    Settings settings;
    settings.baseUrl = "http://127.0.0.1:1";  // 已有模型，不会请求端点
    settings.model = "deepseek-v4-pro";
    AgentSession session(settings, "/tmp/pi-agent-session-test-cwd-thinking");
    session.new_session();
    session.set_model("deepseek-v4-pro");
    session.set_thinking(ThinkingLevel::Off);

    const auto override = Settings::load().override_for("/tmp/pi-agent-session-test-cwd-thinking");
    ASSERT_TRUE(override.has_value());
    ASSERT_TRUE(override->model.has_value());
    EXPECT_EQ(*override->model, "deepseek-v4-pro");
    ASSERT_TRUE(override->thinking.has_value());
    EXPECT_EQ(*override->thinking, ThinkingLevel::Off);
}

}  // namespace
}  // namespace pi

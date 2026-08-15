#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pi/app/settings.h"
#include "pi/harness/agent_harness.h"
#include "pi/harness/env.h"
#include "pi/harness/jsonl_repo.h"

namespace pi
{

/**
 * REPL 会话装配：AgentHarness + 会话仓库 + 成本累计 + 模型/thinking 按 cwd 持久化。
 * 镜像 pi 的 AgentSession。
 */
class AgentSession
{
   public:
    AgentSession(Settings settings, std::string cwd);

    /** 创建新会话（文件落在 ~/.pi-cpp/agent/sessions/<cwd>/...）。 */
    void new_session();
    /** 恢复 cwd 下最新会话；无会话时新建。 */
    void resume();
    /** 按会话 id（精确或前缀）恢复；找不到返回 false 并写入 errorOut。 */
    bool resume_by_id(const std::string& id, std::string* errorOut = nullptr);
    /** 按 /sessions 列表序号（0 = 最新）恢复；越界返回 false。 */
    bool resume_by_index(int index, std::string* errorOut = nullptr);
    /** 当前会话 id（空串表示尚未创建会话）。 */
    std::string current_session_id() const;

    void prompt(const std::string& text, const std::vector<ContentBlock>& images = {});
    void steer(AgentMessage message);
    void follow_up(AgentMessage message);
    void abort();
    void reset();
    bool is_busy() const;
    void wait_for_idle();

    void set_tools(std::vector<AgentTool> tools);
    /** 切换模型（按 cwd 持久化）。 */
    void set_model(const std::string& modelId);
    /** 切换 thinking（按 cwd 持久化）。 */
    void set_thinking(ThinkingLevel level);
    bool compact(std::string* errorOut = nullptr);

    std::vector<SessionMetadata> list_sessions() const;

    std::vector<AgentMessage> messages() const;
    ModelInfo model() const;
    ThinkingLevel thinking_level() const;
    std::string system_prompt() const;
    // 成本/用量由 run 线程（监听器）写、UI 线程读，统一经 cost_mutex_ 保护。
    double total_cost() const;
    double all_time_cost() const;
    Usage last_turn_usage() const;
    /** 当前已解析的 API key（可能为空）。 */
    const std::string& api_key() const { return settings_.apiKey; }
    const WebSearchConfig& web_search_config() const { return settings_.webSearch; }
    const std::string& cwd() const { return cwd_; }
    Session& session() { return harness_.session(); }
    AgentHarness& harness() { return harness_; }

    std::function<void()> subscribe(AgentEventListener listener);

   private:
    std::optional<std::string> resolve_api_key() const;
    ModelInfo resolve_model() const;
    bool open_and_load(const SessionMetadata& metadata, std::string* errorOut);
    void reset_turn_cost();

    Settings settings_;
    std::string cwd_;
    PosixFileSystem fs_;
    JsonlSessionRepo repo_;
    AgentHarness harness_;
    mutable std::mutex cost_mutex_;  // 保护 total_cost_/all_time_cost_/last_turn_usage_
    double total_cost_ = 0;
    double all_time_cost_ = 0;
    Usage last_turn_usage_;
};

}  // namespace pi

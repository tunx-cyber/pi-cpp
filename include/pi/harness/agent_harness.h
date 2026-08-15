#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/agent.h"
#include "pi/harness/compaction.h"
#include "pi/harness/session.h"
#include "pi/harness/types.h"

namespace pi
{

struct AgentHarnessOptions
{
    Session session;
    std::vector<AgentTool> tools;
    std::vector<Skill> skills;
    std::string systemPromptBase;
    std::shared_ptr<TransportAdapter> transport;
    std::function<std::optional<std::string>(const ModelInfo&)> getApiKey;
    ModelInfo model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    QueueMode steeringMode = QueueMode::OneAtATime;
    QueueMode followUpMode = QueueMode::OneAtATime;
    CompactionSettings compactionSettings = default_compaction_settings();
};

/**
 * Agent + Session 装配：消息持久化、自动压缩、模型/thinking 恢复。
 * 镜像 pi 的 AgentHarness（裁剪版）。
 */
class AgentHarness
{
   public:
    explicit AgentHarness(AgentHarnessOptions options);

    /** 从会话恢复上下文（model/thinkingLevel/消息）；等价于 reload()。 */
    void resume();
    /** 无条件重新加载当前会话上下文到 agent（/resume 与切换会话时使用）。 */
    void reload();

    void prompt(const std::string& text);
    void prompt_messages(const std::vector<AgentMessage>& messages);
    void steer(AgentMessage message);
    void follow_up(AgentMessage message);
    void abort();
    bool is_busy() const;
    void wait_for_idle();
    void reset();

    void set_model(const ModelInfo& model);
    void set_thinking_level(ThinkingLevel level);
    void set_tools(std::vector<AgentTool> tools);

    /** 手动压缩（/compact）：threshold 检查只发生在 maybe_auto_compact，此处总是执行。 */
    bool compact(std::string* errorOut = nullptr);

    std::vector<AgentMessage> messages() const;
    std::vector<AgentTool> tools() const;
    ModelInfo model() const;
    ThinkingLevel thinking_level() const;
    std::string system_prompt() const;
    Session& session() { return session_; }
    const Session& session() const { return session_; }

    std::function<void()> subscribe(AgentEventListener listener);
    /** 会话累计成本。run 线程（监听器）写、UI 线程读，由 cost_mutex_ 保护。 */
    double total_cost() const
    {
        std::lock_guard<std::mutex> lock(cost_mutex_);
        return total_cost_;
    }

   private:
    std::string build_system_prompt() const;
    void persist_message(const AgentMessage& message);
    void maybe_auto_compact();
    Result<std::string, CompactionError> run_compaction();

    Session session_;
    Agent agent_;
    std::vector<AgentTool> tools_;
    std::vector<Skill> skills_;
    std::string system_prompt_base_;
    std::shared_ptr<TransportAdapter> transport_;
    std::function<std::optional<std::string>(const ModelInfo&)> get_api_key_;
    ModelInfo model_;
    ThinkingLevel thinking_level_;
    ModelInfo default_model_;             // 构造时确定，reload() 先恢复默认再按会话覆盖
    ThinkingLevel default_thinking_level_;
    CompactionSettings compaction_settings_;
    mutable std::mutex cost_mutex_;  // 保护 total_cost_（跨线程读写）
    double total_cost_ = 0;
};

}  // namespace pi

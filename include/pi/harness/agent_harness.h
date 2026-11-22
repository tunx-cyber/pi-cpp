#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/agent.h"
#include "pi/ai/model_info.h"
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
    void abort();
    void reset();

    void set_model(const ModelInfo& model);
    void set_thinking_level(ThinkingLevel level);
    void set_tools(std::vector<AgentTool> tools);

    /** 手动压缩（/compact）：不再有自动压缩，只有用户显式调用时才执行。 */
    bool compact(std::string* errorOut = nullptr);

    std::vector<AgentMessage> messages() const;
    ModelInfo model() const;
    ThinkingLevel thinking_level() const;
    std::string system_prompt() const;
    Session& session() { return session_; }
    const Session& session() const { return session_; }

    std::function<void()> subscribe(AgentEventListener listener);

   private:
    std::string build_system_prompt() const;
    void persist_message(const AgentMessage& message);
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
    ModelInfo default_model_;  // 构造时确定，reload() 先恢复默认再按会话覆盖
    ThinkingLevel default_thinking_level_;
    CompactionSettings compaction_settings_;
    /** 手动 /compact 的中止信号（abort() 同时设置它，使压缩可取消）。 */
    mutable std::mutex compaction_mutex_;
    std::shared_ptr<std::atomic<bool>> compaction_abort_;
};

}  // namespace pi

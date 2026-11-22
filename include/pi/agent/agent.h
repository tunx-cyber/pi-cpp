#pragma once

#include <condition_variable>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/agent_loop.h"
#include "pi/agent/pending_queue.h"

namespace pi
{

using AgentEventListener =
    std::function<void(const AgentEvent&, const std::shared_ptr<std::atomic<bool>>&)>;

/** Agent 构造选项，镜像 pi 的 AgentOptions。 */
struct AgentOptions
{
    std::optional<std::string> systemPrompt;
    std::optional<ModelInfo> model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::vector<AgentTool> tools;
    std::vector<AgentMessage> messages;
    std::shared_ptr<TransportAdapter> transport;
    std::function<std::optional<std::string>(const std::string& provider)> getApiKey;
    std::function<bool(const ShouldStopAfterTurnContext&)> shouldStopAfterTurn;
};

/**
 * 有状态 agent，镜像 pi 的 Agent。
 * prompt/continue 同步阻塞执行（调用线程 = run 线程）；busy 时抛错。
 * steer/followUp 线程安全入队；abort 通过共享 atomic 贯穿 transport 与工具。
 * 事件只在 run 线程派发。
 */
class Agent
{
   public:
    Agent(AgentOptions options = {});

    /** 订阅事件；返回取消函数。监听器异常会报告为运行失败；失败通知再次抛异常时向调用者传播。 */
    std::function<void()> subscribe(AgentEventListener listener);

    std::vector<AgentMessage> messages() const;
    std::string system_prompt() const;
    ModelInfo model() const;
    ThinkingLevel thinking_level() const;

    void set_tools(std::vector<AgentTool> tools);
    void set_messages(std::vector<AgentMessage> messages);
    void set_system_prompt(std::string prompt);
    void set_model(ModelInfo model);
    void set_thinking_level(ThinkingLevel level);
    void set_transport(std::shared_ptr<TransportAdapter> transport);

    void steer(AgentMessage message);

    void abort();
    bool is_busy() const;
    void wait_for_idle();

    void reset();

    /** 从文本/消息启动新 prompt；busy 时抛 std::runtime_error。 */
    void prompt(const std::string& text);
    void prompt_messages(const std::vector<AgentMessage>& messages);

   private:
    AgentLoopConfig make_loop_config();
    void run_with_lifecycle(
        const std::function<void(const std::shared_ptr<std::atomic<bool>>&)>& executor);
    void process_events(const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>& signal);
    void handle_run_failure(const std::exception& error, bool aborted,
                            const std::shared_ptr<std::atomic<bool>>& signal);

    std::string system_prompt_;
    ModelInfo model_;
    ThinkingLevel thinking_level_ = ThinkingLevel::Off;
    std::vector<AgentTool> tools_;
    std::vector<AgentMessage> messages_;
    PendingMessageQueue steering_queue_;
    std::vector<AgentEventListener> listeners_;

    mutable std::mutex state_mutex_;
    std::shared_ptr<std::atomic<bool>> abort_;
    bool busy_ = false;
    mutable std::mutex busy_mutex_;
    std::condition_variable idle_cv_;

    std::shared_ptr<TransportAdapter> transport_;
    std::function<std::optional<std::string>(const std::string&)> get_api_key_;
    std::function<bool(const ShouldStopAfterTurnContext&)> should_stop_after_turn_;
};

}  // namespace pi

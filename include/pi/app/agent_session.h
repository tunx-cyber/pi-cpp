#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pi/ai/model_list.h"
#include "pi/app/settings.h"
#include "pi/harness/agent_harness.h"
#include "pi/harness/env.h"
#include "pi/harness/jsonl_repo.h"

namespace pi
{

/**
 * REPL 会话装配：AgentHarness + 会话仓库 + token 用量累计 + 模型/thinking 按 cwd 持久化。
 * 镜像 pi 的 AgentSession。价格随时变动，因此只累计四项 token，不累计金额。
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
    void abort();

    void set_tools(std::vector<AgentTool> tools);
    /** 切换模型（按 cwd 持久化）。注册表里没有的 id（如 /models 返回的远端模型）按自定义模型接入。
     */
    void set_model(const std::string& modelId);
    /**
     * 确定当前模型：已有模型（会话记录 / settings.json / PI_MODEL）时直接返回；
     * 否则取端点 `GET /models` 返回的第一个模型作为默认模型（不写入 settings.json）。
     * 失败时返回 false 并写入 errorOut（端点不可达 / 未返回任何模型）。
     * 前置条件：会话已创建或恢复（需要向会话写入模型切换记录）。
     */
    /**
     * 确定当前模型：总是向端点 `GET /models` 探测一次。
     * - 已配置模型出现在端点列表中 → 保留；
     * - 不在列表里（换了 baseUrl、或旧会话记录的模型）→ 改用端点第一个模型，
     *   提示写入 noticeOut（只改进程内状态，不落盘）；
     * - 端点不可用 → 沿用已配置模型，只有连一个模型都没有时才失败。
     */
    bool ensure_model(std::string* errorOut = nullptr, std::string* noticeOut = nullptr);
    /** 切换 thinking（按 cwd 持久化）。 */
    void set_thinking(ThinkingLevel level);
    bool compact(std::string* errorOut = nullptr);

    /** 通过 OpenAI 兼容端点 `GET {baseUrl}/models` 获取可用模型（不缓存、不抛异常）。 */
    RemoteModelList list_models() const;

    std::vector<SessionMetadata> list_sessions() const;

    std::vector<AgentMessage> messages() const;
    ModelInfo model() const;
    ThinkingLevel thinking_level() const;
    std::string system_prompt() const;
    // 用量由 run 线程（监听器）写、UI 线程读，统一经 usage_mutex_ 保护。
    /** 当前会话累计 token 用量（/new、/resume、/clear 后清零）。 */
    Usage session_usage() const;
    /** 本进程累计 token 用量（跨会话，重启清零）。 */
    Usage all_time_usage() const;
    /** 最近一次 LLM 调用的用量（含工具循环中的中间调用）。 */
    Usage last_turn_usage() const;
    /** 当前已解析的 API key（可能为空）。 */
    const std::string& api_key() const { return settings_.apiKey; }
    /** OpenAI 兼容端点（/models 与 chat/completions 共用）。 */
    const std::string& base_url() const { return settings_.baseUrl; }
    const WebSearchConfig& web_search_config() const { return settings_.webSearch; }

    std::function<void()> subscribe(AgentEventListener listener);

   private:
    std::optional<std::string> resolve_api_key() const;
    /** 已配置的模型（settings.json / byCwd / PI_MODEL）；未配置返回 nullopt。 */
    std::optional<ModelInfo> resolve_model() const;
    bool open_and_load(const SessionMetadata& metadata, std::string* errorOut);
    void reset_session_usage();

    Settings settings_;
    std::string cwd_;
    /** ensure_model 自动探测出的模型 id：只在进程内有效，不写入 settings.json。 */
    std::string current_model_id_;
    PosixFileSystem fs_;
    JsonlSessionRepo repo_;
    AgentHarness harness_;
    mutable std::mutex usage_mutex_;  // 保护 session_usage_/all_time_usage_/last_turn_usage_
    Usage session_usage_;
    Usage all_time_usage_;
    Usage last_turn_usage_;
};

}  // namespace pi

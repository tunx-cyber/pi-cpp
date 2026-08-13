#include "pi/agent/subagent_tool.h"

#include <chrono>

#include <thread>

#include "pi/agent/agent.h"

namespace pi
{

namespace
{

/** 构造纯文本 ToolResult（与错误状态无关，成功/失败路径共用）。 */
ToolResult text_result(const std::string& message)
{
    ToolResult result;
    result.content.push_back(ContentBlock{});
    result.content.back().type = BlockType::Text;
    result.content.back().text = message;
    return result;
}

AgentTool build_subagent_tool(const SubagentToolOptions& options, int depth);

/** 在父工具线程上运行子 agent（内联，不跳线程）。 */
ToolResult run_subagent(const SubagentToolOptions& options, const std::string& prompt, int depth,
                        const std::shared_ptr<std::atomic<bool>>& signal,
                        const std::function<void(const ToolResult&)>& onUpdate)
{
    if (depth > options.maxNestingDepth)
    {
        return text_result("Error: max nesting depth exceeded (" +
                           std::to_string(options.maxNestingDepth) + ")");
    }

    AgentOptions agent_options;
    agent_options.systemPrompt = options.systemPrompt;
    agent_options.model = options.model;
    agent_options.thinkingLevel = options.thinkingLevel.value_or(ThinkingLevel::Off);
    agent_options.transport = options.transport;
    agent_options.getApiKey = options.getApiKey;

    // 递归子工具（depth+1）
    std::vector<AgentTool> sub_tools = options.tools;
    sub_tools.push_back(build_subagent_tool(options, depth + 1));
    agent_options.tools = std::move(sub_tools);

    // maxTurns 防护：每轮 turn_end 计数
    auto turns_used = std::make_shared<int>(0);
    agent_options.shouldStopAfterTurn =
        [turns_used, max_turns = options.maxTurns](const ShouldStopAfterTurnContext&) -> bool
    { return ++(*turns_used) >= max_turns; };

    Agent sub_agent(agent_options);

    // 子 agent 流式文本转发为父的 tool_execution_update
    sub_agent.subscribe(
        [onUpdate](const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
        {
            if (event.type == AgentEvent::Type::MessageUpdate && event.assistantMessageEvent &&
                onUpdate)
            {
                const auto& stream_event = *event.assistantMessageEvent;
                if (stream_event.type == StreamEvent::Type::TextDelta)
                {
                    ToolResult partial;
                    partial.content.push_back(ContentBlock{});
                    partial.content.back().type = BlockType::Text;
                    partial.content.back().text = stream_event.delta;
                    onUpdate(partial);
                }
            }
        });

    // abort 链：共享父工具的 signal（prompt 前已 abort 则直接失败）
    if (signal && signal->load())
    {
        return text_result("Error: subagent aborted");
    }
    auto run_finished = std::make_shared<std::atomic<bool>>(false);
    std::thread abort_watcher(
        [&, run_finished]
        {
            while (true)
            {
                if (signal && signal->load())
                {
                    sub_agent.abort();
                    break;
                }
                if (run_finished->load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

    try
    {
        sub_agent.prompt(prompt);
    }
    catch (const std::exception& e)
    {
        run_finished->store(true);
        if (abort_watcher.joinable()) abort_watcher.join();
        return text_result("Error: subagent failed: " + std::string(e.what()));
    }
    run_finished->store(true);
    if (abort_watcher.joinable()) abort_watcher.join();

    if (*turns_used >= options.maxTurns)
    {
        // 轮次上限命中时，只有当最后一轮仍在请求工具（工作被截断）才报错；
        // 若最后一条 assistant 消息是干净的最终回答，则照常返回。
        const auto& messages = sub_agent.messages();
        bool cut_off = false;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        {
            if (it->role == Role::Assistant)
            {
                cut_off = it->has_tool_calls();
                break;
            }
        }
        if (cut_off)
        {
            return text_result("Error: max turns exceeded (" + std::to_string(options.maxTurns) +
                               ")");
        }
    }

    // 汇总子 agent 最后一条成功的 assistant 文本
    std::string text;
    const auto& messages = sub_agent.messages();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        if (it->role == Role::Assistant && it->stopReason != StopReason::Error &&
            it->stopReason != StopReason::Aborted)
        {
            text = it->text_content();
            break;
        }
    }
    if (text.empty())
    {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        {
            if (it->role == Role::Assistant && !it->text_content().empty())
            {
                text = it->text_content();
                break;
            }
        }
    }
    return text_result("<subagent>\n" + text + "\n</subagent>");
}

AgentTool build_subagent_tool(const SubagentToolOptions& options, int depth)
{
    AgentTool tool;
    tool.name = "subagent";
    tool.description =
        "Run a sub-agent to complete a focused subtask. The sub-agent has its own model, tools, "
        "and "
        "conversation. Returns the sub-agent's final answer wrapped in <subagent> tags.";
    tool.label = "subagent";
    tool.executionMode = ToolExecutionMode::Sequential;
    tool.parameters =
        Json{{"type", "object"},
             {"properties",
              Json{{"prompt", Json{{"type", "string"}, {"description", "Subtask to complete"}}}}},
             {"required", Json::array({"prompt"})}};
    tool.execute = [options, depth](
                       const std::string&, const Json& args,
                       const std::shared_ptr<std::atomic<bool>>& signal,
                       const std::function<void(const ToolResult&)>& onUpdate) -> ToolResult
    { return run_subagent(options, args.value("prompt", ""), depth, signal, onUpdate); };
    return tool;
}

}  // namespace

AgentTool make_subagent_tool(SubagentToolOptions options)
{
    return build_subagent_tool(std::move(options), 1);
}

}  // namespace pi

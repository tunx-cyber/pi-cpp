#include "pi/agent/agent_loop.h"

#include <cctype>

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>

#include "pi/agent/json_schema.h"
#include "pi/ai/json_util.h"
#include "pi/util/scope_exit.h"

namespace pi
{

namespace
{

bool tool_names_equal(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
}

constexpr int kMaxConcurrentTools = 8;

struct PreparedToolCall
{
    const ContentBlock* toolCall;
    const AgentTool* tool;
    Json args;
};

struct FinalizedToolCallOutcome
{
    const ContentBlock* toolCall;
    ToolResult result;
    bool isError = false;
};

ToolResult create_error_tool_result(const std::string& message)
{
    ToolResult result;
    ContentBlock block;
    block.type = BlockType::Text;
    block.text = message;
    result.content.push_back(std::move(block));
    return result;
}

bool should_terminate_tool_batch(const std::vector<FinalizedToolCallOutcome>& finalized)
{
    if (finalized.empty()) return false;
    for (const auto& outcome : finalized)
    {
        if (!outcome.result.terminate) return false;
    }
    return true;
}

Message create_tool_result_message(const FinalizedToolCallOutcome& finalized)
{
    Message message;
    message.role = Role::ToolResult;
    message.toolCallId = finalized.toolCall->id;
    message.toolName = finalized.toolCall->name;
    message.content = finalized.result.content;
    message.details = finalized.result.details;
    message.isError = finalized.isError;
    return message;
}

void emit_tool_result_message(const Message& message, const AgentEventSink& emit)
{
    AgentEvent start;
    start.type = AgentEvent::Type::MessageStart;
    start.message = message;
    emit(start);
    AgentEvent end;
    end.type = AgentEvent::Type::MessageEnd;
    end.message = message;
    emit(end);
}

/** 工具调用准备（preflight）：查工具 + schema 校验。失败返回 immediate outcome。 */
struct PreparationResult
{
    bool immediate = false;
    ToolResult result;
    bool isError = false;
    PreparedToolCall prepared;
};

PreparationResult prepare_tool_call(const AgentContext& context, const ContentBlock& tool_call,
                                    const std::shared_ptr<std::atomic<bool>>& signal)
{
    const AgentTool* tool = nullptr;
    for (const auto& t : context.tools)
    {
        if (tool_names_equal(t.name, tool_call.name))
        {
            tool = &t;
            break;
        }
    }
    if (!tool)
    {
        return {true, create_error_tool_result("Tool " + tool_call.name + " not found"), true, {}};
    }

    try
    {
        PreparedToolCall prepared{&tool_call, tool, tool_call.arguments};
        const auto validation_error = validate_tool_args(*tool, prepared.args);
        if (validation_error)
        {
            return {true, create_error_tool_result(*validation_error), true, {}};
        }
        if (signal && signal->load())
        {
            return {true, create_error_tool_result("Operation aborted"), true, {}};
        }
        return {false, {}, false, std::move(prepared)};
    }
    catch (const std::exception& e)
    {
        return {true, create_error_tool_result(e.what()), true, {}};
    }
}

struct ExecutedToolBatch
{
    std::vector<Message> messages;
    bool terminate = false;
};

bool has_sequential_tool_call(const std::vector<ContentBlock>& tool_calls,
                              const AgentContext& context)
{
    for (const auto& call : tool_calls)
    {
        for (const auto& tool : context.tools)
        {
            if (tool_names_equal(tool.name, call.name) &&
                tool.executionMode == ToolExecutionMode::Sequential)
                return true;
        }
    }
    return false;
}

ExecutedToolBatch execute_tool_calls_sequential(AgentContext& context,
                                                const AgentMessage& assistant_message,
                                                const std::vector<ContentBlock>& tool_calls,
                                                AgentLoopConfig& config, const AgentEventSink& emit,
                                                const std::shared_ptr<std::atomic<bool>>& signal)
{
    std::vector<FinalizedToolCallOutcome> finalized_calls;
    std::vector<Message> messages;

    for (const auto& tool_call : tool_calls)
    {
        AgentEvent start;
        start.type = AgentEvent::Type::ToolExecutionStart;
        start.toolCallId = tool_call.id;
        start.toolName = tool_call.name;
        start.args = tool_call.arguments;
        emit(start);

        const auto preparation = prepare_tool_call(context, tool_call, signal);
        FinalizedToolCallOutcome finalized;
        if (preparation.immediate)
        {
            finalized.toolCall = &tool_call;
            finalized.result = preparation.result;
            finalized.isError = preparation.isError;
        }
        else
        {
            FinalizedToolCallOutcome executed_outcome;
            executed_outcome.toolCall = &tool_call;
            try
            {
                executed_outcome.result = preparation.prepared.tool->execute(
                    tool_call.id, preparation.prepared.args, signal);
                executed_outcome.isError = false;
            }
            catch (const std::exception& e)
            {
                // 与并行路径保持一致：工具 execute 抛异常（如 LLM 传错参数类型触发
                // get<type>()）转为 error toolResult，而不是终结整轮对话。
                executed_outcome.result = create_error_tool_result(e.what());
                executed_outcome.isError = true;
            }
            finalized = executed_outcome;
        }

        AgentEvent end;
        end.type = AgentEvent::Type::ToolExecutionEnd;
        end.toolCallId = tool_call.id;
        end.toolName = tool_call.name;
        end.args = tool_call.arguments;
        end.result = finalized.result;
        end.isError = finalized.isError;
        emit(end);

        const auto tool_result_message = create_tool_result_message(finalized);
        emit_tool_result_message(tool_result_message, emit);
        finalized_calls.push_back(std::move(finalized));
        messages.push_back(std::move(tool_result_message));

        if (signal && signal->load()) break;
    }

    return {std::move(messages), should_terminate_tool_batch(finalized_calls)};
}

ExecutedToolBatch execute_tool_calls_parallel(AgentContext& context,
                                              const AgentMessage& assistant_message,
                                              const std::vector<ContentBlock>& tool_calls,
                                              AgentLoopConfig& config, const AgentEventSink& emit,
                                              const std::shared_ptr<std::atomic<bool>>& signal)
{
    struct Entry
    {
        bool immediate = false;
        FinalizedToolCallOutcome outcome;
        PreparedToolCall prepared;
    };

    std::vector<Entry> entries;
    for (const auto& tool_call : tool_calls)
    {
        AgentEvent start;
        start.type = AgentEvent::Type::ToolExecutionStart;
        start.toolCallId = tool_call.id;
        start.toolName = tool_call.name;
        start.args = tool_call.arguments;
        emit(start);

        const auto preparation = prepare_tool_call(context, tool_call, signal);
        Entry entry;
        if (preparation.immediate)
        {
            entry.immediate = true;
            entry.outcome.toolCall = &tool_call;
            entry.outcome.result = preparation.result;
            entry.outcome.isError = preparation.isError;
        }
        else
        {
            entry.prepared = std::move(preparation.prepared);
        }
        entries.push_back(std::move(entry));
        if (signal && signal->load()) break;
    }

    // Workers execute tools only. Hooks and event dispatch stay on the run thread.
    std::vector<FinalizedToolCallOutcome> results(entries.size());
    std::vector<size_t> completion_order;
    completion_order.reserve(entries.size());
    std::mutex completion_mutex;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr worker_error;

    auto worker = [&]
    {
        try
        {
            while (!failed.load())
            {
                const size_t i = next.fetch_add(1);
                if (i >= entries.size()) return;
                auto& executed = results[i];
                if (entries[i].immediate)
                {
                    executed = entries[i].outcome;
                }
                else
                {
                    executed.toolCall = entries[i].prepared.toolCall;
                    try
                    {
                        if (signal && signal->load())
                        {
                            executed.result = create_error_tool_result("Operation aborted");
                            executed.isError = true;
                        }
                        else
                        {
                            executed.result = entries[i].prepared.tool->execute(
                                executed.toolCall->id, entries[i].prepared.args, signal);
                        }
                    }
                    catch (const std::exception& error)
                    {
                        executed.result = create_error_tool_result(error.what());
                        executed.isError = true;
                    }
                    catch (...)
                    {
                        executed.result = create_error_tool_result("Unknown tool exception");
                        executed.isError = true;
                    }
                }
                std::lock_guard<std::mutex> lock(completion_mutex);
                completion_order.push_back(i);
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(completion_mutex);
            if (!worker_error) worker_error = std::current_exception();
            failed.store(true);
        }
    };

    std::vector<std::thread> workers;
    const size_t worker_count = std::min(entries.size(), static_cast<size_t>(kMaxConcurrentTools));
    workers.reserve(worker_count);
    {
        ScopeExit join_workers(
            [&]() noexcept
            {
                for (auto& worker_thread : workers)
                    if (worker_thread.joinable()) worker_thread.join();
            });
        try
        {
            for (size_t i = 0; i < worker_count; ++i) workers.emplace_back(worker);
        }
        catch (...)
        {
            failed.store(true);
            throw;
        }
    }
    if (worker_error) std::rethrow_exception(worker_error);

    // R1 按完成序先发收集到的流式更新，再发 tool_execution_end
    for (size_t index : completion_order)
    {
        const auto& outcome = results[index];
        AgentEvent end;
        end.type = AgentEvent::Type::ToolExecutionEnd;
        end.toolCallId = outcome.toolCall->id;
        end.toolName = outcome.toolCall->name;
        end.args = outcome.toolCall->arguments;
        end.result = outcome.result;
        end.isError = outcome.isError;
        emit(end);
    }

    // toolResult 消息按 assistant 源序
    std::vector<Message> messages;
    for (auto& outcome : results)
    {
        const auto tool_result_message = create_tool_result_message(outcome);
        emit_tool_result_message(tool_result_message, emit);
        messages.push_back(std::move(tool_result_message));
    }

    return {std::move(messages), should_terminate_tool_batch(results)};
}

ExecutedToolBatch execute_tool_calls(AgentContext& context, const AgentMessage& assistant_message,
                                     const std::vector<ContentBlock>& tool_calls,
                                     AgentLoopConfig& config, const AgentEventSink& emit,
                                     const std::shared_ptr<std::atomic<bool>>& signal)
{
    const bool sequential = has_sequential_tool_call(tool_calls, context);
    if (sequential)
    {
        return execute_tool_calls_sequential(context, assistant_message, tool_calls, config, emit,
                                             signal);
    }
    return execute_tool_calls_parallel(context, assistant_message, tool_calls, config, emit,
                                       signal);
}

/** 流式 assistant 响应：LLM 调用 + 事件状态机，镜像 streamAssistantResponse。 */
Message stream_assistant_response(AgentContext& context, AgentLoopConfig& config,
                                  const std::shared_ptr<std::atomic<bool>>& signal,
                                  const AgentEventSink& emit)
{
    if (!config.transport)
    {
        // 防御：Agent 未配置 transport 时不再空指针解引用，而是以 error 事件终止本轮。
        Message failure;
        failure.role = Role::Assistant;
        failure.api = config.model.api;
        failure.provider = config.model.provider;
        failure.model = config.model.id;
        failure.stopReason = StopReason::Error;
        failure.errorMessage = "No transport configured";
        AgentEvent start;
        start.type = AgentEvent::Type::MessageStart;
        start.message = failure;
        emit(start);
        AgentEvent end;
        end.type = AgentEvent::Type::MessageEnd;
        end.message = failure;
        emit(end);
        return failure;
    }

    // Role 只有 user/assistant/toolResult 三种，无需再过滤拷贝：直接引用 context
    const std::vector<AgentMessage>& llm_messages = context.messages;

    std::optional<std::string> api_key;
    if (config.getApiKey)
    {
        try
        {
            api_key = config.getApiKey(config.model.provider);
        }
        catch (...)
        {
        }
    }

    StreamRequestOptions opts;
    opts.apiKey = api_key;
    opts.reasoning = config.reasoning;
    opts.systemPrompt = context.systemPrompt;
    std::vector<Tool> wire_tools;
    for (const auto& tool : context.tools)
    {
        Tool wire_tool;
        wire_tool.name = tool.name;
        wire_tool.description = tool.description;
        wire_tool.parameters = tool.parameters;
        wire_tools.push_back(std::move(wire_tool));
    }
    opts.tools = std::move(wire_tools);
    opts.abort = signal;

    Message final_message;
    bool added_partial = false;
    bool message_done = false;

    config.transport->stream_chat(
        config.model, llm_messages, opts,
        [&](const StreamEvent& event)
        {
            switch (event.type)
            {
                case StreamEvent::Type::Start:
                    final_message = event.message;
                    context.messages.push_back(final_message);
                    added_partial = true;
                    {
                        AgentEvent agent_event;
                        agent_event.type = AgentEvent::Type::MessageStart;
                        agent_event.message = final_message;
                        emit(agent_event);
                    }
                    break;

                case StreamEvent::Type::TextStart:
                case StreamEvent::Type::TextEnd:
                case StreamEvent::Type::ThinkingStart:
                case StreamEvent::Type::ThinkingEnd:
                case StreamEvent::Type::ToolCallStart:
                case StreamEvent::Type::ToolCallEnd:
                    // 块边界事件携带完整部分消息：同步到 transcript
                    if (added_partial)
                    {
                        final_message = event.message;
                        if (!context.messages.empty()) context.messages.back() = final_message;
                        AgentEvent agent_event;
                        agent_event.type = AgentEvent::Type::MessageUpdate;
                        agent_event.message = final_message;
                        agent_event.assistantMessageEvent = event;
                        emit(agent_event);
                    }
                    break;

                case StreamEvent::Type::TextDelta:
                case StreamEvent::Type::ThinkingDelta:
                case StreamEvent::Type::ToolCallDelta:
                    // delta 不带整条 message（否则每个 token 复制一次
                    // 已累积的回复，O(n²)）：只把增量透传给 UI。
                    if (added_partial)
                    {
                        AgentEvent agent_event;
                        agent_event.type = AgentEvent::Type::MessageUpdate;
                        agent_event.assistantMessageEvent = event;
                        emit(agent_event);
                    }
                    break;

                case StreamEvent::Type::Done:
                case StreamEvent::Type::Error:
                    final_message = event.message;
                    if (added_partial)
                    {
                        if (!context.messages.empty()) context.messages.back() = final_message;
                    }
                    else
                    {
                        context.messages.push_back(final_message);
                    }
                    if (!added_partial)
                    {
                        AgentEvent agent_event;
                        agent_event.type = AgentEvent::Type::MessageStart;
                        agent_event.message = final_message;
                        emit(agent_event);
                    }
                    {
                        AgentEvent agent_event;
                        agent_event.type = AgentEvent::Type::MessageEnd;
                        agent_event.message = final_message;
                        emit(agent_event);
                    }
                    message_done = true;
                    break;
            }
        });

    if (!message_done)
    {
        // 传输未发终止事件（不应发生；防御处理）
        if (added_partial)
        {
            if (!context.messages.empty()) context.messages.back() = final_message;
        }
        else
        {
            context.messages.push_back(final_message);
        }
        AgentEvent agent_event;
        agent_event.type = AgentEvent::Type::MessageEnd;
        agent_event.message = final_message;
        emit(agent_event);
    }
    return final_message;
}

void run_loop(AgentContext& current_context, std::vector<AgentMessage>& new_messages,
              AgentLoopConfig& config, const std::shared_ptr<std::atomic<bool>>& signal,
              const AgentEventSink& emit)
{
    bool first_turn = true;
    std::vector<AgentMessage> pending_messages;
    if (config.getSteeringMessages)
    {
        try
        {
            pending_messages = config.getSteeringMessages();
        }
        catch (...)
        {
        }
    }

    while (true)
    {
        bool has_more_tool_calls = true;

        while (has_more_tool_calls || !pending_messages.empty())
        {
            if (!first_turn)
            {
                AgentEvent turn_start;
                turn_start.type = AgentEvent::Type::TurnStart;
                emit(turn_start);
            }
            else
            {
                first_turn = false;
            }

            if (!pending_messages.empty())
            {
                for (const auto& message : pending_messages)
                {
                    AgentEvent start;
                    start.type = AgentEvent::Type::MessageStart;
                    start.message = message;
                    emit(start);
                    AgentEvent end;
                    end.type = AgentEvent::Type::MessageEnd;
                    end.message = message;
                    emit(end);
                    current_context.messages.push_back(message);
                    new_messages.push_back(message);
                }
                pending_messages.clear();
            }

            const Message message =
                stream_assistant_response(current_context, config, signal, emit);
            new_messages.push_back(message);

            if (message.stopReason == StopReason::Error ||
                message.stopReason == StopReason::Aborted)
            {
                AgentEvent turn_end;
                turn_end.type = AgentEvent::Type::TurnEnd;
                turn_end.message = message;
                emit(turn_end);
                AgentEvent agent_end;
                agent_end.type = AgentEvent::Type::AgentEnd;
                emit(agent_end);
                return;
            }

            std::vector<ContentBlock> tool_calls;
            for (const auto& block : message.content)
            {
                if (block.type == BlockType::ToolCall) tool_calls.push_back(block);
            }

            std::vector<Message> tool_results;
            has_more_tool_calls = false;
            if (!tool_calls.empty())
            {
                const auto batch =
                    execute_tool_calls(current_context, message, tool_calls, config, emit, signal);
                tool_results = batch.messages;
                has_more_tool_calls = !batch.terminate;
                for (const auto& result : tool_results)
                {
                    current_context.messages.push_back(result);
                    new_messages.push_back(result);
                }
            }

            AgentEvent turn_end;
            turn_end.type = AgentEvent::Type::TurnEnd;
            turn_end.message = message;
            emit(turn_end);

            if (config.shouldStopAfterTurn)
            {
                ShouldStopAfterTurnContext stop_context;
                stop_context.message = &message;
                stop_context.toolResults = &tool_results;
                stop_context.context = &current_context;
                stop_context.newMessages = &new_messages;
                bool stop = false;
                try
                {
                    stop = config.shouldStopAfterTurn(stop_context);
                }
                catch (...)
                {
                }
                if (stop)
                {
                    AgentEvent agent_end;
                    agent_end.type = AgentEvent::Type::AgentEnd;
                    emit(agent_end);
                    return;
                }
            }

            pending_messages.clear();
            if (config.getSteeringMessages)
            {
                try
                {
                    pending_messages = config.getSteeringMessages();
                }
                catch (...)
                {
                }
            }
        }
        break;
    }

    AgentEvent agent_end;
    agent_end.type = AgentEvent::Type::AgentEnd;
    emit(agent_end);
}

}  // namespace

std::vector<AgentMessage> run_agent_loop(const std::vector<AgentMessage>& prompts,
                                         AgentContext& context, AgentLoopConfig& config,
                                         const AgentEventSink& emit,
                                         const std::shared_ptr<std::atomic<bool>>& signal)
{
    std::vector<AgentMessage> new_messages = prompts;
    for (const auto& prompt : prompts)
    {
        context.messages.push_back(prompt);
    }

    AgentEvent agent_start;
    agent_start.type = AgentEvent::Type::AgentStart;
    emit(agent_start);
    AgentEvent turn_start;
    turn_start.type = AgentEvent::Type::TurnStart;
    emit(turn_start);
    for (const auto& prompt : prompts)
    {
        AgentEvent start;
        start.type = AgentEvent::Type::MessageStart;
        start.message = prompt;
        emit(start);
        AgentEvent end;
        end.type = AgentEvent::Type::MessageEnd;
        end.message = prompt;
        emit(end);
    }

    run_loop(context, new_messages, config, signal, emit);
    return new_messages;
}

}  // namespace pi

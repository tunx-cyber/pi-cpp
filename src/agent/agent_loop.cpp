#include "pi/agent/agent_loop.h"

#include <condition_variable>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <thread>

#include "pi/agent/json_schema.h"
#include "pi/ai/json_util.h"

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

/**
 * 工具调用准备（preflight）：查工具、prepareArguments、schema 校验、beforeToolCall 钩子。
 * 失败/拦截返回 immediate outcome。
 */
struct PreparationResult
{
    bool immediate = false;
    ToolResult result;
    bool isError = false;
    PreparedToolCall prepared;
};

PreparationResult prepare_tool_call(const AgentContext& context,
                                    const AgentMessage& assistant_message,
                                    const ContentBlock& tool_call, AgentLoopConfig& config,
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
        if (tool->prepareArguments)
        {
            Json prepared_args = tool->prepareArguments(tool_call.arguments);
            if (prepared_args.is_object()) prepared.args = std::move(prepared_args);
        }
        const auto validation_error = validate_tool_args(*tool, prepared.args);
        if (validation_error)
        {
            return {true, create_error_tool_result(*validation_error), true, {}};
        }
        if (config.beforeToolCall)
        {
            BeforeToolCallContext hook_context;
            hook_context.assistantMessage = &assistant_message;
            hook_context.toolCall = &tool_call;
            hook_context.args = prepared.args;
            hook_context.context = const_cast<AgentContext*>(&context);
            const auto before_result = config.beforeToolCall(hook_context, signal);
            if (signal && signal->load())
            {
                return {true, create_error_tool_result("Operation aborted"), true, {}};
            }
            if (before_result.block)
            {
                return {true,
                        create_error_tool_result(before_result.reason.empty()
                                                     ? "Tool execution was blocked"
                                                     : before_result.reason),
                        true,
                        {}};
            }
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

FinalizedToolCallOutcome finalize_executed_tool_call(
    const AgentContext& context, const AgentMessage& assistant_message,
    const PreparedToolCall& prepared, const FinalizedToolCallOutcome& executed,
    AgentLoopConfig& config, const std::shared_ptr<std::atomic<bool>>& signal)
{
    FinalizedToolCallOutcome outcome = executed;
    if (!config.afterToolCall) return outcome;

    try
    {
        AfterToolCallContext hook_context;
        hook_context.assistantMessage = &assistant_message;
        hook_context.toolCall = prepared.toolCall;
        hook_context.args = prepared.args;
        hook_context.result = outcome.result;
        hook_context.isError = outcome.isError;
        hook_context.context = const_cast<AgentContext*>(&context);
        const auto after = config.afterToolCall(hook_context, signal);
        if (after.hasContent) outcome.result.content = after.content;
        if (after.hasDetails) outcome.result.details = after.details;
        if (after.hasIsError) outcome.isError = after.isError;
        if (after.hasTerminate) outcome.result.terminate = after.terminate;
    }
    catch (const std::exception& e)
    {
        outcome.result = create_error_tool_result(e.what());
        outcome.isError = true;
    }
    return outcome;
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

        const auto preparation =
            prepare_tool_call(context, assistant_message, tool_call, config, signal);
        FinalizedToolCallOutcome finalized;
        if (preparation.immediate)
        {
            finalized.toolCall = &tool_call;
            finalized.result = preparation.result;
            finalized.isError = preparation.isError;
        }
        else
        {
            const ToolResult executed = preparation.prepared.tool->execute(
                tool_call.id, preparation.prepared.args, signal,
                [&](const ToolResult& partial)
                {
                    AgentEvent update;
                    update.type = AgentEvent::Type::ToolExecutionUpdate;
                    update.toolCallId = tool_call.id;
                    update.toolName = tool_call.name;
                    update.args = tool_call.arguments;
                    update.partialResult = partial;
                    emit(update);
                });
            FinalizedToolCallOutcome executed_outcome;
            executed_outcome.toolCall = &tool_call;
            executed_outcome.result = executed;
            executed_outcome.isError = false;
            finalized = finalize_executed_tool_call(
                context, assistant_message, preparation.prepared, executed_outcome, config, signal);
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

        const auto preparation =
            prepare_tool_call(context, assistant_message, tool_call, config, signal);
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

    // 并发执行：每调用一个线程，上限 8 并发；完成序记录在 completion_order。
    // 并行工具的流式更新由工具线程收集（completion_mutex 保护），R1 在完成序阶段统一发出。
    std::vector<FinalizedToolCallOutcome> results(entries.size());
    std::vector<std::vector<ToolResult>> updates(entries.size());
    std::vector<size_t> completion_order;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    std::atomic<int> active{0};

    std::vector<std::thread> threads;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].immediate)
        {
            results[i] = entries[i].outcome;
            {
                std::lock_guard<std::mutex> lock(completion_mutex);
                completion_order.push_back(i);
            }
            continue;
        }
        threads.emplace_back(
            [&, i]
            {
                {
                    std::unique_lock<std::mutex> lock(completion_mutex);
                    completion_cv.wait(lock, [&] { return active.load() < kMaxConcurrentTools; });
                    active.fetch_add(1);
                }
                FinalizedToolCallOutcome executed;
                executed.toolCall = entries[i].prepared.toolCall;
                try
                {
                    const ToolResult result = entries[i].prepared.tool->execute(
                        entries[i].prepared.toolCall->id, entries[i].prepared.args, signal,
                        [&updates, i, &completion_mutex](const ToolResult& partial)
                        {
                            std::lock_guard<std::mutex> lock(completion_mutex);
                            updates[i].push_back(partial);
                        });
                    executed.result = result;
                    executed.isError = false;
                }
                catch (const std::exception& e)
                {
                    executed.result = create_error_tool_result(e.what());
                    executed.isError = true;
                }
                auto finalized = finalize_executed_tool_call(
                    context, assistant_message, entries[i].prepared, executed, config, signal);
                {
                    std::lock_guard<std::mutex> lock(completion_mutex);
                    results[i] = std::move(finalized);
                    completion_order.push_back(i);
                    active.fetch_sub(1);
                    completion_cv.notify_one();
                }
            });
    }
    for (auto& thread : threads) thread.join();

    // R1 按完成序先发收集到的流式更新，再发 tool_execution_end
    for (size_t index : completion_order)
    {
        for (const auto& partial : updates[index])
        {
            AgentEvent update;
            update.type = AgentEvent::Type::ToolExecutionUpdate;
            update.toolCallId = results[index].toolCall->id;
            update.toolName = results[index].toolCall->name;
            update.args = results[index].toolCall->arguments;
            update.partialResult = partial;
            emit(update);
        }
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
    const bool sequential = config.toolExecution == ToolExecutionMode::Sequential ||
                            has_sequential_tool_call(tool_calls, context);
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
    std::vector<AgentMessage> messages = context.messages;
    if (config.transformContext)
    {
        try
        {
            messages = config.transformContext(messages, signal);
        }
        catch (...)
        {
        }
    }

    std::vector<AgentMessage> llm_messages;
    if (config.convertToLlm)
    {
        try
        {
            llm_messages = config.convertToLlm(messages);
        }
        catch (...)
        {
            llm_messages = default_convert_to_llm(messages);
        }
    }
    else
    {
        llm_messages = default_convert_to_llm(messages);
    }

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

    config.transport->stream_chat(config.model, llm_messages, opts,
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
                                          case StreamEvent::Type::TextDelta:
                                          case StreamEvent::Type::TextEnd:
                                          case StreamEvent::Type::ThinkingStart:
                                          case StreamEvent::Type::ThinkingDelta:
                                          case StreamEvent::Type::ThinkingEnd:
                                          case StreamEvent::Type::ToolCallStart:
                                          case StreamEvent::Type::ToolCallDelta:
                                          case StreamEvent::Type::ToolCallEnd:
                                              if (added_partial)
                                              {
                                                  final_message = event.message;
                                                  if (!context.messages.empty())
                                                      context.messages.back() = final_message;
                                                  AgentEvent agent_event;
                                                  agent_event.type =
                                                      AgentEvent::Type::MessageUpdate;
                                                  agent_event.message = final_message;
                                                  agent_event.assistantMessageEvent = event;
                                                  emit(agent_event);
                                              }
                                              break;

                                          case StreamEvent::Type::Done:
                                          case StreamEvent::Type::Error:
                                              final_message = event.message;
                                              if (added_partial)
                                              {
                                                  if (!context.messages.empty())
                                                      context.messages.back() = final_message;
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
                turn_end.toolResults = {};
                emit(turn_end);
                AgentEvent agent_end;
                agent_end.type = AgentEvent::Type::AgentEnd;
                agent_end.messages = new_messages;
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
            turn_end.toolResults = tool_results;
            emit(turn_end);

            if (config.prepareNextTurn)
            {
                ShouldStopAfterTurnContext next_turn_context;
                next_turn_context.message = &message;
                next_turn_context.toolResults = &tool_results;
                next_turn_context.context = &current_context;
                next_turn_context.newMessages = &new_messages;
                try
                {
                    const auto update = config.prepareNextTurn(next_turn_context);
                    if (update.context) current_context = *update.context;
                    if (update.model) config.model = *update.model;
                    if (update.thinkingLevel)
                    {
                        config.reasoning =
                            *update.thinkingLevel == ThinkingLevel::Off
                                ? std::optional<ThinkingLevel>(std::nullopt)
                                : std::optional<ThinkingLevel>(*update.thinkingLevel);
                    }
                }
                catch (...)
                {
                }
            }

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
                    agent_end.messages = new_messages;
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

        // agent 本应停止；检查 follow-up 队列
        std::vector<AgentMessage> follow_up_messages;
        if (config.getFollowUpMessages)
        {
            try
            {
                follow_up_messages = config.getFollowUpMessages();
            }
            catch (...)
            {
            }
        }
        if (!follow_up_messages.empty())
        {
            pending_messages = std::move(follow_up_messages);
            continue;
        }
        break;
    }

    AgentEvent agent_end;
    agent_end.type = AgentEvent::Type::AgentEnd;
    agent_end.messages = new_messages;
    emit(agent_end);
}

}  // namespace

std::vector<AgentMessage> default_convert_to_llm(const std::vector<AgentMessage>& messages)
{
    std::vector<AgentMessage> out;
    for (const auto& message : messages)
    {
        if (message.role == Role::User || message.role == Role::Assistant ||
            message.role == Role::ToolResult)
        {
            out.push_back(message);
        }
    }
    return out;
}

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

std::vector<AgentMessage> run_agent_loop_continue(AgentContext& context, AgentLoopConfig& config,
                                                  const AgentEventSink& emit,
                                                  const std::shared_ptr<std::atomic<bool>>& signal)
{
    std::vector<AgentMessage> new_messages;

    AgentEvent agent_start;
    agent_start.type = AgentEvent::Type::AgentStart;
    emit(agent_start);
    AgentEvent turn_start;
    turn_start.type = AgentEvent::Type::TurnStart;
    emit(turn_start);

    run_loop(context, new_messages, config, signal, emit);
    return new_messages;
}

}  // namespace pi

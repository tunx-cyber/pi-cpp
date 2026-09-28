#include "pi/agent/agent.h"

#include <exception>
#include <stdexcept>

#include "pi/util/scope_exit.h"

namespace pi
{

Agent::Agent(AgentOptions options)
    : model_(options.model.value_or(ModelInfo{})),
      thinking_level_(options.thinkingLevel),
      tools_(std::move(options.tools)),
      messages_(std::move(options.messages)),
      steering_queue_(options.steeringMode),
      follow_up_queue_(options.followUpMode),
      transport_(std::move(options.transport)),
      get_api_key_(std::move(options.getApiKey)),
      convert_to_llm_(std::move(options.convertToLlm)),
      transform_context_(std::move(options.transformContext)),
      before_tool_call_(std::move(options.beforeToolCall)),
      after_tool_call_(std::move(options.afterToolCall)),
      prepare_next_turn_(std::move(options.prepareNextTurn)),
      should_stop_after_turn_(std::move(options.shouldStopAfterTurn)),
      tool_execution_(options.toolExecution)
{
    if (options.systemPrompt) system_prompt_ = *options.systemPrompt;
}

std::function<void()> Agent::subscribe(AgentEventListener listener)
{
    auto alive = std::make_shared<std::atomic<bool>>(true);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        listeners_.push_back(
            [alive, listener = std::move(listener)](
                const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>& signal)
            {
                if (alive->load()) listener(event, signal);
            });
    }
    return [alive] { alive->store(false); };
}

bool Agent::is_streaming() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return is_streaming_;
}

std::optional<AgentMessage> Agent::streaming_message() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return streaming_message_;
}

std::set<std::string> Agent::pending_tool_calls() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pending_tool_calls_;
}

std::string Agent::error_message() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return error_message_;
}

std::vector<AgentMessage> Agent::messages() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return messages_;
}

std::vector<AgentTool> Agent::tools() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return tools_;
}

std::string Agent::system_prompt() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_prompt_;
}

ModelInfo Agent::model() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return model_;
}

ThinkingLevel Agent::thinking_level() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return thinking_level_;
}

void Agent::set_tools(std::vector<AgentTool> tools)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    tools_ = std::move(tools);
}

void Agent::set_messages(std::vector<AgentMessage> messages)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    messages_ = std::move(messages);
}

void Agent::set_system_prompt(std::string prompt)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    system_prompt_ = std::move(prompt);
}

void Agent::set_model(ModelInfo model)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    model_ = std::move(model);
}

void Agent::set_thinking_level(ThinkingLevel level)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    thinking_level_ = level;
}

void Agent::set_transport(std::shared_ptr<TransportAdapter> transport)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    transport_ = std::move(transport);
}

void Agent::steer(AgentMessage message) { steering_queue_.enqueue(std::move(message)); }

void Agent::follow_up(AgentMessage message) { follow_up_queue_.enqueue(std::move(message)); }

void Agent::clear_steering_queue() { steering_queue_.clear(); }

void Agent::clear_follow_up_queue() { follow_up_queue_.clear(); }

void Agent::clear_all_queues()
{
    clear_steering_queue();
    clear_follow_up_queue();
}

bool Agent::has_queued_messages() const
{
    return steering_queue_.has_items() || follow_up_queue_.has_items();
}

void Agent::abort()
{
    std::shared_ptr<std::atomic<bool>> signal;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        signal = abort_;
    }
    if (signal) signal->store(true);
}

bool Agent::is_busy() const
{
    std::lock_guard<std::mutex> lock(busy_mutex_);
    return busy_;
}

void Agent::wait_for_idle()
{
    std::unique_lock<std::mutex> lock(busy_mutex_);
    idle_cv_.wait(lock, [this] { return !busy_; });
}

void Agent::reset()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    messages_.clear();
    is_streaming_ = false;
    streaming_message_.reset();
    pending_tool_calls_.clear();
    error_message_.clear();
    follow_up_queue_.clear();
    steering_queue_.clear();
}

void Agent::prompt(const std::string& text)
{
    std::vector<AgentMessage> messages;
    messages.push_back(Message::user(text));
    prompt_messages(messages);
}

void Agent::prompt_messages(const std::vector<AgentMessage>& messages)
{
    if (is_busy())
    {
        throw std::runtime_error(
            "Agent is already processing a prompt. Use steer() or followUp() to queue messages, or "
            "wait for "
            "completion.");
    }
    std::vector<AgentMessage> prompts = messages;
    run_with_lifecycle(
        [this, prompts = std::move(prompts)](const std::shared_ptr<std::atomic<bool>>& signal)
        {
            AgentContext context;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                context.systemPrompt = system_prompt_;
                context.messages = messages_;
                context.tools = tools_;
            }
            auto config = make_loop_config();
            run_agent_loop(
                prompts, context, config, [this, &signal](const AgentEvent& event)
                { process_events(event, signal); }, signal);
        });
}

void Agent::continue_run()
{
    if (is_busy())
    {
        throw std::runtime_error(
            "Agent is already processing. Wait for completion before continuing.");
    }
    std::vector<AgentMessage> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = messages_;
    }
    if (snapshot.empty())
    {
        throw std::runtime_error("No messages to continue from");
    }
    if (snapshot.back().role == Role::Assistant)
    {
        const auto steering = steering_queue_.drain();
        if (!steering.empty())
        {
            prompt_messages(steering);
            return;
        }
        const auto follow_ups = follow_up_queue_.drain();
        if (!follow_ups.empty())
        {
            prompt_messages(follow_ups);
            return;
        }
        throw std::runtime_error("Cannot continue from message role: assistant");
    }
    run_with_lifecycle(
        [this](const std::shared_ptr<std::atomic<bool>>& signal)
        {
            AgentContext context;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                context.systemPrompt = system_prompt_;
                context.messages = messages_;
                context.tools = tools_;
            }
            auto config = make_loop_config();
            run_agent_loop_continue(
                context, config, [this, &signal](const AgentEvent& event)
                { process_events(event, signal); }, signal);
        });
}

AgentLoopConfig Agent::make_loop_config()
{
    AgentLoopConfig config;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        config.model = model_;
        config.reasoning = thinking_level_ == ThinkingLevel::Off
                               ? std::optional<ThinkingLevel>(std::nullopt)
                               : std::optional<ThinkingLevel>(thinking_level_);
        config.transport = transport_;
        config.getApiKey = get_api_key_;
        config.toolExecution = tool_execution_;
        config.convertToLlm = convert_to_llm_;
        config.transformContext = transform_context_;
        config.beforeToolCall = before_tool_call_;
        config.afterToolCall = after_tool_call_;
        config.prepareNextTurn = prepare_next_turn_;
        config.shouldStopAfterTurn = should_stop_after_turn_;
    }
    config.getSteeringMessages = [this]() { return steering_queue_.drain(); };
    config.getFollowUpMessages = [this]() { return follow_up_queue_.drain(); };
    return config;
}

void Agent::run_with_lifecycle(
    const std::function<void(const std::shared_ptr<std::atomic<bool>>&)>& executor)
{
    {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        if (busy_)
        {
            throw std::runtime_error("Agent is already processing.");
        }
        busy_ = true;
    }
    ScopeExit restore_idle(
        [this]() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                is_streaming_ = false;
                streaming_message_.reset();
                pending_tool_calls_.clear();
            }
            {
                std::lock_guard<std::mutex> lock(busy_mutex_);
                busy_ = false;
            }
            idle_cv_.notify_all();
        });
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        abort_ = std::make_shared<std::atomic<bool>>(false);
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        is_streaming_ = true;
        streaming_message_.reset();
        error_message_.clear();
    }

    try
    {
        executor(abort_);
    }
    catch (const std::exception& e)
    {
        handle_run_failure(e, abort_->load(), abort_);
    }
    catch (...)
    {
        handle_run_failure(std::runtime_error("unknown error"), abort_->load(), abort_);
    }
}

void Agent::handle_run_failure(const std::exception& error, bool aborted,
                               const std::shared_ptr<std::atomic<bool>>& signal)
{
    // model_ 可能被 UI 线程的 set_model 并发修改，先取快照
    ModelInfo model_snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        model_snapshot = model_;
        error_message_ = error.what();
    }

    Message failure;
    failure.role = Role::Assistant;
    ContentBlock block;
    block.type = BlockType::Text;
    failure.content.push_back(std::move(block));
    failure.api = model_snapshot.api;
    failure.provider = model_snapshot.provider;
    failure.model = model_snapshot.id;
    failure.stopReason = aborted ? StopReason::Aborted : StopReason::Error;
    failure.errorMessage = error.what();

    AgentEvent message_start;
    message_start.type = AgentEvent::Type::MessageStart;
    message_start.message = failure;
    process_events(message_start, signal);
    AgentEvent message_end;
    message_end.type = AgentEvent::Type::MessageEnd;
    message_end.message = failure;
    process_events(message_end, signal);
    AgentEvent turn_end;
    turn_end.type = AgentEvent::Type::TurnEnd;
    turn_end.message = failure;
    process_events(turn_end, signal);
    AgentEvent agent_end;
    agent_end.type = AgentEvent::Type::AgentEnd;
    agent_end.messages = {failure};
    process_events(agent_end, signal);
}

void Agent::process_events(const AgentEvent& event,
                           const std::shared_ptr<std::atomic<bool>>& signal)
{
    switch (event.type)
    {
        case AgentEvent::Type::MessageStart:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            streaming_message_ = event.message;
            break;
        }
        case AgentEvent::Type::MessageUpdate:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            streaming_message_ = event.message;
            break;
        }
        case AgentEvent::Type::MessageEnd:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            streaming_message_.reset();
            messages_.push_back(event.message);
            break;
        }
        case AgentEvent::Type::ToolExecutionStart:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pending_tool_calls_.insert(event.toolCallId);
            break;
        }
        case AgentEvent::Type::ToolExecutionEnd:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pending_tool_calls_.erase(event.toolCallId);
            break;
        }
        case AgentEvent::Type::TurnEnd:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (event.message.role == Role::Assistant && !event.message.errorMessage.empty())
            {
                error_message_ = event.message.errorMessage;
            }
            break;
        }
        case AgentEvent::Type::AgentEnd:
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            streaming_message_.reset();
            break;
        }
        default:
            break;
    }

    // 监听器在 run 线程按订阅顺序同步调用
    std::vector<AgentEventListener> listeners;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        listeners = listeners_;
    }
    // Notify every subscriber even if one fails. The run boundary reports the first failure.
    std::exception_ptr listener_error;
    for (const auto& listener : listeners)
    {
        try
        {
            listener(event, signal);
        }
        catch (...)
        {
            if (!listener_error) listener_error = std::current_exception();
        }
    }
    if (listener_error) std::rethrow_exception(listener_error);
}

}  // namespace pi

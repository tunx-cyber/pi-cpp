#include "pi/harness/agent_harness.h"

#include "pi/ai/model_registry.h"
#include "pi/harness/system_prompt.h"

namespace pi
{

AgentHarness::AgentHarness(AgentHarnessOptions options)
    : session_(std::move(options.session)),
      tools_(std::move(options.tools)),
      skills_(std::move(options.skills)),
      system_prompt_base_(std::move(options.systemPromptBase)),
      transport_(std::move(options.transport)),
      get_api_key_(std::move(options.getApiKey)),
      model_(std::move(options.model)),
      thinking_level_(options.thinkingLevel),
      compaction_settings_(options.compactionSettings)
{
    agent_.set_system_prompt(build_system_prompt());
    agent_.set_model(model_);
    agent_.set_thinking_level(thinking_level_);
    agent_.set_tools(tools_);
    agent_.set_transport(transport_);

    // 消息持久化 + 成本累计
    agent_.subscribe(
        [this](const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
        {
            if (event.type == AgentEvent::Type::MessageEnd)
            {
                persist_message(event.message);
            }
            if (event.type == AgentEvent::Type::MessageEnd &&
                event.message.role == Role::Assistant &&
                event.message.stopReason != StopReason::Error &&
                event.message.stopReason != StopReason::Aborted)
            {
                std::lock_guard<std::mutex> lock(cost_mutex_);
                total_cost_ += event.message.usage.cost.total;
            }
        });
}

std::string AgentHarness::build_system_prompt() const
{
    return pi::build_system_prompt(system_prompt_base_, skills_);
}

void AgentHarness::resume()
{
    if (resumed_) return;
    resumed_ = true;
    const auto context = session_.build_context();
    if (!context.ok) return;
    const auto& ctx = context.value;
    if (ctx.model)
    {
        const auto model = get_model(ctx.model->second);
        if (model)
        {
            model_ = *model;
            agent_.set_model(model_);
        }
    }
    const auto thinking = thinking_level_from_string(ctx.thinkingLevel);
    if (thinking)
    {
        thinking_level_ = *thinking;
        agent_.set_thinking_level(thinking_level_);
    }
    if (ctx.activeToolNames)
    {
        std::vector<AgentTool> active;
        for (const auto& tool : tools_)
        {
            for (const auto& name : *ctx.activeToolNames)
            {
                if (tool.name == name)
                {
                    active.push_back(tool);
                    break;
                }
            }
        }
        if (!active.empty())
        {
            agent_.set_tools(active);
        }
    }
    if (!ctx.messages.empty())
    {
        agent_.set_messages(ctx.messages);
    }
}

void AgentHarness::prompt(const std::string& text)
{
    agent_.prompt(text);
    maybe_auto_compact();
}

void AgentHarness::prompt_messages(const std::vector<AgentMessage>& messages)
{
    agent_.prompt_messages(messages);
    maybe_auto_compact();
}

void AgentHarness::steer(AgentMessage message) { agent_.steer(std::move(message)); }

void AgentHarness::follow_up(AgentMessage message) { agent_.follow_up(std::move(message)); }

void AgentHarness::abort() { agent_.abort(); }

bool AgentHarness::is_busy() const { return agent_.is_busy(); }

void AgentHarness::wait_for_idle() { agent_.wait_for_idle(); }

void AgentHarness::reset()
{
    agent_.reset();
    std::lock_guard<std::mutex> lock(cost_mutex_);
    total_cost_ = 0;
}

void AgentHarness::set_model(const ModelInfo& model)
{
    model_ = model;
    agent_.set_model(model);
    session_.append_model_change(model.provider, model.id);
}

void AgentHarness::set_thinking_level(ThinkingLevel level)
{
    thinking_level_ = level;
    agent_.set_thinking_level(level);
    session_.append_thinking_level_change(to_string(level));
}

void AgentHarness::set_tools(std::vector<AgentTool> tools)
{
    tools_ = std::move(tools);
    agent_.set_tools(tools_);
    std::vector<std::string> names;
    for (const auto& tool : tools_) names.push_back(tool.name);
    session_.append_active_tools_change(names);
}

void AgentHarness::persist_message(const AgentMessage& message)
{
    session_.append_message(message);
}

void AgentHarness::maybe_auto_compact()
{
    if (!compaction_settings_.enabled) return;
    const auto branch = session_.get_branch();
    if (!branch.ok) return;
    std::vector<AgentMessage> messages;
    for (const auto& entry : branch.value)
    {
        if (entry.type == SessionTreeEntry::Type::Message) messages.push_back(entry.message);
    }
    const auto estimate = estimate_context_tokens(messages);
    if (!should_compact(estimate.tokens, model_.contextWindow, compaction_settings_)) return;

    std::string error;
    compact(&error);
}

Result<std::string, CompactionError> AgentHarness::run_compaction()
{
    const auto branch = session_.get_branch();
    if (!branch.ok)
    {
        CompactionError error;
        error.code = CompactionErrorCode::InvalidSession;
        error.message = branch.error.message;
        return Result<std::string, CompactionError>::err_value(error);
    }
    const auto preparation = prepare_compaction(branch.value, compaction_settings_);
    if (!preparation.ok) return Result<std::string, CompactionError>::err_value(preparation.error);
    if (!preparation.value.has_value())
    {
        return Result<std::string, CompactionError>::err_value(
            CompactionError{CompactionErrorCode::Unknown, "Nothing to compact"});
    }

    std::optional<std::string> api_key;
    if (get_api_key_)
    {
        try
        {
            api_key = get_api_key_(model_);
        }
        catch (...)
        {
        }
    }

    const auto result = pi::compact(*preparation.value, model_, transport_, api_key,
                                    std::make_shared<std::atomic<bool>>(false));
    if (!result.ok) return Result<std::string, CompactionError>::err_value(result.error);

    const auto appended =
        session_.append_compaction(result.value.summary, result.value.firstKeptEntryId,
                                   result.value.tokensBefore, result.value.details, false);
    if (!appended.ok)
    {
        CompactionError error;
        error.code = CompactionErrorCode::InvalidSession;
        error.message = appended.error.message;
        return Result<std::string, CompactionError>::err_value(error);
    }

    // 重放压缩后的上下文到 agent
    const auto context = session_.build_context();
    if (context.ok)
    {
        agent_.set_messages(context.value.messages);
    }
    return Result<std::string, CompactionError>::ok_value(result.value.summary);
}

bool AgentHarness::compact(std::string* errorOut)
{
    const auto result = run_compaction();
    if (!result.ok)
    {
        if (errorOut) *errorOut = result.error.message;
        return false;
    }
    return true;
}

std::vector<AgentMessage> AgentHarness::messages() const { return agent_.messages(); }

std::vector<AgentTool> AgentHarness::tools() const { return agent_.tools(); }

ModelInfo AgentHarness::model() const { return agent_.model(); }

ThinkingLevel AgentHarness::thinking_level() const { return agent_.thinking_level(); }

std::string AgentHarness::system_prompt() const { return agent_.system_prompt(); }

std::function<void()> AgentHarness::subscribe(AgentEventListener listener)
{
    return agent_.subscribe(std::move(listener));
}

}  // namespace pi

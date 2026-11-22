#include "pi/harness/agent_harness.h"

#include <stdexcept>

#include "pi/ai/model_registry.h"
#include "pi/harness/system_prompt.h"

namespace pi
{
namespace
{
template <typename T>
void require_session_write(const Result<T, SessionError>& result)
{
    if (!result.ok) throw std::runtime_error("Session storage failed: " + result.error.message);
}
}  // namespace

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
    default_model_ = model_;
    default_thinking_level_ = thinking_level_;
    agent_.set_system_prompt(build_system_prompt());
    agent_.set_model(model_);
    agent_.set_thinking_level(thinking_level_);
    agent_.set_tools(tools_);
    agent_.set_transport(transport_);

    // 消息持久化（用量随消息一起落盘）
    agent_.subscribe(
        [this](const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
        {
            if (event.type == AgentEvent::Type::MessageEnd)
            {
                persist_message(event.message);
            }
        });
}

std::string AgentHarness::build_system_prompt() const
{
    return pi::build_system_prompt(system_prompt_base_, skills_);
}

void AgentHarness::resume() { reload(); }

void AgentHarness::reload()
{
    // 先恢复构造时的默认模型/thinking，再按会话记录覆盖：避免从"改过模型的会话"
    // 切到"未记录模型/thinking 的会话"时残留上一会话的模型与 thinking。
    model_ = default_model_;
    thinking_level_ = default_thinking_level_;
    agent_.set_model(model_);
    agent_.set_thinking_level(thinking_level_);

    const auto context = session_.build_context();
    require_session_write(context);
    const auto& ctx = context.value;
    if (ctx.model)
    {
        const auto model = get_model(ctx.model->second);
        if (model)
        {
            model_ = *model;
            agent_.set_model(model_);
        }
        else
        {
            // 会话记录的是运行时从 GET /models 选中的远端模型（注册表里没有元数据）：
            // 先按 id/端点推断同族能力模板（reasoning/thinking 映射/上下文窗口），
            // 再退回默认模型结构；两种情况都只把 id 换回会话记录值。
            // 保留默认 provider/baseUrl，避免 custom provider 用模型自带 baseUrl
            // 绕过全局 PI_BASE_URL 覆盖。
            ModelInfo remote = default_model_;
            if (const auto template_model =
                    find_model_template(ctx.model->second, default_model_.baseUrl))
            {
                remote = *template_model;
                if (!default_model_.baseUrl.empty()) remote.baseUrl = default_model_.baseUrl;
            }
            remote.id = ctx.model->second;
            remote.name = ctx.model->second;
            model_ = remote;
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
        agent_.set_tools(std::move(active));
    }
    // 总是设置消息（即使为空），保证切换到空会话时清空 transcript。
    // 此前 /resume 有 resumed_ 守卫，二次调用是 no-op；现在 reload() 可重复执行。
    agent_.set_messages(ctx.messages);
}

void AgentHarness::prompt(const std::string& text) { agent_.prompt(text); }

void AgentHarness::prompt_messages(const std::vector<AgentMessage>& messages)
{
    agent_.prompt_messages(messages);
}

void AgentHarness::steer(AgentMessage message) { agent_.steer(std::move(message)); }

void AgentHarness::abort()
{
    {
        std::lock_guard<std::mutex> lock(compaction_mutex_);
        if (compaction_abort_) compaction_abort_->store(true);
    }
    agent_.abort();
}

void AgentHarness::reset() { agent_.reset(); }

void AgentHarness::set_model(const ModelInfo& model)
{
    require_session_write(session_.append_model_change(model.provider, model.id));
    model_ = model;
    // 切换后的模型同时成为默认模型：reload() 用它恢复"没有模型记录"的会话，
    // 也保证启动时自动探测出的模型在 /resume、/new 之后仍然生效。
    default_model_ = model;
    agent_.set_model(model_);
}

void AgentHarness::set_thinking_level(ThinkingLevel level)
{
    require_session_write(session_.append_thinking_level_change(to_string(level)));
    thinking_level_ = level;
    agent_.set_thinking_level(level);
}

void AgentHarness::set_tools(std::vector<AgentTool> tools)
{
    std::vector<std::string> names;
    for (const auto& tool : tools) names.push_back(tool.name);
    require_session_write(session_.append_active_tools_change(names));
    tools_ = std::move(tools);
    agent_.set_tools(tools_);
}

void AgentHarness::persist_message(const AgentMessage& message)
{
    require_session_write(session_.append_message(message));
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

    // 压缩走一次真实 LLM 调用：暴露中止信号，Esc 时 abort() 能立即打断。
    auto compaction_signal = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(compaction_mutex_);
        compaction_abort_ = compaction_signal;
    }
    const auto result =
        pi::compact(*preparation.value, model_, transport_, api_key, compaction_signal);
    {
        std::lock_guard<std::mutex> lock(compaction_mutex_);
        compaction_abort_.reset();
    }
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

ModelInfo AgentHarness::model() const { return agent_.model(); }

ThinkingLevel AgentHarness::thinking_level() const { return agent_.thinking_level(); }

std::string AgentHarness::system_prompt() const { return agent_.system_prompt(); }

std::function<void()> AgentHarness::subscribe(AgentEventListener listener)
{
    return agent_.subscribe(std::move(listener));
}

}  // namespace pi

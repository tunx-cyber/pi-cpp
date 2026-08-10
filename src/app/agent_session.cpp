#include "pi/app/agent_session.h"

#include <unistd.h>

#include "pi/agent/subagent_tool.h"
#include "pi/ai/model_registry.h"

namespace pi
{

namespace
{

AgentHarnessOptions build_harness_options(const Settings& settings, const std::string& cwd)
{
    AgentHarnessOptions options;
    std::string model_id = settings.model;
    if (const auto override = settings.override_for(cwd))
    {
        if (override->model) model_id = *override->model;
    }
    const auto model = get_model(model_id);
    if (model)
    {
        options.model = *model;
    }
    else
    {
        ModelInfo custom;
        custom.id = model_id;
        custom.name = model_id;
        custom.api = "openai-completions";
        custom.provider = "custom";
        custom.baseUrl = settings.baseUrl;
        custom.reasoning = false;
        custom.input = {"text", "image"};
        custom.contextWindow = 131072;
        custom.maxTokens = 32768;
        options.model = custom;
    }
    options.thinkingLevel = settings.thinking;
    options.transport = make_openai_completions_transport(settings.baseUrl, settings.apiKey);
    options.getApiKey = [&settings](const ModelInfo&) -> std::optional<std::string>
    {
        return settings.apiKey.empty() ? std::nullopt : std::optional<std::string>(settings.apiKey);
    };
    options.systemPromptBase = "You are pi-cpp, a helpful coding assistant in a terminal.";
    return options;
}

}  // namespace

AgentSession::AgentSession(Settings settings, std::string cwd)
    : settings_(std::move(settings)),
      cwd_(std::move(cwd)),
      fs_(cwd_),
      repo_(fs_, Settings::expand_home(settings_.sessionsRoot)),
      harness_(build_harness_options(settings_, cwd_))
{
    harness_.subscribe(
        [this](const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
        {
            if (event.type == AgentEvent::Type::MessageEnd &&
                event.message.role == Role::Assistant &&
                event.message.stopReason != StopReason::Error &&
                event.message.stopReason != StopReason::Aborted)
            {
                total_cost_ += event.message.usage.cost.total;
                all_time_cost_ += event.message.usage.cost.total;
                last_turn_usage_ = event.message.usage;
            }
        });
}

std::optional<std::string> AgentSession::resolve_api_key() const
{
    if (!settings_.apiKey.empty()) return settings_.apiKey;
    return std::nullopt;
}

ModelInfo AgentSession::resolve_model() const
{
    std::string model_id = settings_.model;
    if (const auto override = settings_.override_for(cwd_))
    {
        if (override->model) model_id = *override->model;
    }
    const auto model = get_model(model_id);
    if (model) return *model;
    // 未知模型：构造自定义模型（用户端点场景）
    ModelInfo custom;
    custom.id = model_id;
    custom.name = model_id;
    custom.api = "openai-completions";
    custom.provider = "custom";
    custom.baseUrl = settings_.baseUrl;
    custom.reasoning = false;
    custom.input = {"text", "image"};
    custom.contextWindow = 131072;
    custom.maxTokens = 32768;
    return custom;
}

void AgentSession::new_session()
{
    const auto created = repo_.create(cwd_);
    if (!created.ok) return;
    harness_.session() = created.value;
    harness_.set_model(resolve_model());
    harness_.set_thinking_level(settings_.thinking);
    total_cost_ = 0;
    last_turn_usage_ = Usage{};
}

void AgentSession::resume()
{
    const auto sessions = repo_.list(cwd_);
    if (sessions.ok && !sessions.value.empty())
    {
        const auto opened = repo_.open(sessions.value[0]);
        if (opened.ok)
        {
            harness_.session() = opened.value;
            harness_.resume();
            return;
        }
    }
    new_session();
}

void AgentSession::prompt(const std::string& text, const std::vector<ContentBlock>& images)
{
    if (images.empty())
    {
        harness_.prompt(text);
        return;
    }
    Message user;
    user.role = Role::User;
    ContentBlock text_block;
    text_block.type = BlockType::Text;
    text_block.text = text;
    user.content.push_back(std::move(text_block));
    for (const auto& image : images) user.content.push_back(image);
    std::vector<AgentMessage> messages;
    messages.push_back(std::move(user));
    harness_.prompt_messages(messages);
}

void AgentSession::steer(AgentMessage message) { harness_.steer(std::move(message)); }

void AgentSession::follow_up(AgentMessage message) { harness_.follow_up(std::move(message)); }

void AgentSession::abort() { harness_.abort(); }

void AgentSession::reset()
{
    harness_.reset();
    total_cost_ = 0;
    last_turn_usage_ = Usage{};
}

bool AgentSession::is_busy() const { return harness_.is_busy(); }

void AgentSession::wait_for_idle() { harness_.wait_for_idle(); }

void AgentSession::set_tools(std::vector<AgentTool> tools)
{
    // 附加子 agent 工具（子 agent 有独立的 transport，深度/轮次防护）
    SubagentToolOptions sub_options;
    sub_options.model = harness_.model();
    sub_options.thinkingLevel = harness_.thinking_level();
    sub_options.systemPrompt =
        "You are a focused sub-agent. Complete the assigned subtask and report the result "
        "concisely.";
    sub_options.transport =
        make_openai_completions_transport(settings_.baseUrl, resolve_api_key().value_or(""));
    sub_options.getApiKey = [this](const std::string&) { return resolve_api_key(); };
    sub_options.tools = tools;
    tools.push_back(make_subagent_tool(std::move(sub_options)));
    harness_.set_tools(std::move(tools));
}

void AgentSession::set_model(const std::string& modelId)
{
    const auto model = get_model(modelId);
    if (!model) return;
    settings_.model = modelId;
    settings_.set_override(cwd_, modelId, std::nullopt);
    harness_.set_model(*model);
    // 切换模型后，当前 thinking 级别可能不再被支持：自动回退到最近支持的级别，
    // 保证"模型 ↔ thinking 级别"约束始终成立（/thinking 命令与设置文件同理）。
    const auto current = harness_.thinking_level();
    const auto clamped = clamp_thinking_level(*model, current);
    if (clamped != current)
    {
        settings_.thinking = clamped;
        settings_.set_override(cwd_, std::nullopt, clamped);
        harness_.set_thinking_level(clamped);
    }
}

void AgentSession::set_thinking(ThinkingLevel level)
{
    settings_.thinking = level;
    settings_.set_override(cwd_, std::nullopt, level);
    harness_.set_thinking_level(level);
}

bool AgentSession::compact(bool force, std::string* errorOut)
{
    return harness_.compact(force, errorOut);
}

std::vector<SessionMetadata> AgentSession::list_sessions() const
{
    const auto sessions = repo_.list(cwd_);
    if (!sessions.ok) return {};
    return sessions.value;
}

const std::vector<AgentMessage>& AgentSession::messages() const { return harness_.messages(); }

ModelInfo AgentSession::model() const { return harness_.model(); }

ThinkingLevel AgentSession::thinking_level() const { return harness_.thinking_level(); }

std::string AgentSession::system_prompt() const { return harness_.system_prompt(); }

std::function<void()> AgentSession::subscribe(AgentEventListener listener)
{
    return harness_.subscribe(std::move(listener));
}

}  // namespace pi

#include "pi/app/agent_session.h"

#include <unistd.h>

#include <cctype>

#include <algorithm>
#include <stdexcept>

#include "pi/agent/subagent_tool.h"
#include "pi/ai/model_registry.h"
#include "pi/harness/skills.h"

namespace pi
{

namespace
{

/**
 * 注册表里没有的模型（GET /models 返回的远端模型）。
 * 优先继承同族模型的能力模板：端点只返回 id，不带 reasoning/上下文窗口，
 * 若一律按纯文本模型处理，支持思考的模型（如 deepseek-flash）就会被错误限制成
 * thinking=off。找不到同族模型时才退回保守默认值。
 */
ModelInfo make_custom_model(const std::string& model_id, const std::string& base_url)
{
    if (const auto template_model = find_model_template(model_id, base_url))
    {
        ModelInfo custom = *template_model;
        custom.id = model_id;
        custom.name = model_id;
        custom.baseUrl = base_url;
        return custom;
    }

    ModelInfo custom;
    custom.id = model_id;
    custom.name = model_id;
    custom.api = "openai-completions";
    custom.provider = "custom";
    custom.baseUrl = base_url;
    custom.reasoning = false;
    custom.input = {"text", "image"};
    custom.contextWindow = 131072;
    custom.maxTokens = 32768;
    return custom;
}

/**
 * 端点可能把语音/向量等非对话模型一起列出来（例如 mimo-v2.5-asr、*-tts-* 排在前面）。
 * 自动选默认模型时跳过这些明显不是 chat 的条目，否则第一次请求就 400。
 */
bool looks_like_chat_model(const std::string& id)
{
    static const char* const kNonChat[] = {"asr",   "tts",    "whisper",    "voice", "speech",
                                           "embed", "rerank", "moderation", "ocr",   "transcri"};
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* needle : kNonChat)
    {
        if (lower.find(needle) != std::string::npos) return false;
    }
    return true;
}

/** 把一轮消息的用量累加到 target（只统计四项 token，不累计金额）。 */
void accumulate_usage(Usage& target, const Usage& delta)
{
    target.input += delta.input;
    target.output += delta.output;
    target.cacheRead += delta.cacheRead;
    target.cacheWrite += delta.cacheWrite;
    target.totalTokens += delta.totalTokens;
}

AgentHarnessOptions build_harness_options(const Settings& settings, const std::string& cwd)
{
    AgentHarnessOptions options;
    // 未指定模型（PI_MODEL）时保持空 ModelInfo：真正的默认模型由 ensure_model() 通过
    // GET /models 确定（代码里不写死任何模型 id）。
    if (!settings.model.empty())
    {
        if (const auto model = get_model(settings.model))
        {
            options.model = *model;
        }
        else
        {
            options.model = make_custom_model(settings.model, settings.baseUrl);
        }
    }
    options.thinkingLevel = settings.thinking;
    options.transport = make_openai_completions_transport(settings.baseUrl, settings.apiKey);
    options.getApiKey = [&settings](const ModelInfo&) -> std::optional<std::string> {
        return settings.apiKey.empty() ? std::nullopt : std::optional<std::string>(settings.apiKey);
    };
    options.systemPromptBase = settings.systemPrompt;
    // 技能清单写进 system prompt（模型按 location 自行 read SKILL.md）。
    // 此前 options.skills 从未赋值：/skills 只打印列表，模型完全看不到技能。
    PosixFileSystem fs(cwd);
    options.skills = load_skills(fs, {Settings::expand_home("~/.pi-cpp/skills")}).skills;
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
                std::lock_guard<std::mutex> lock(usage_mutex_);
                accumulate_usage(session_usage_, event.message.usage);
                accumulate_usage(all_time_usage_, event.message.usage);
                last_turn_usage_ = event.message.usage;
            }
        });
}

std::optional<std::string> AgentSession::resolve_api_key() const
{
    if (!settings_.apiKey.empty()) return settings_.apiKey;
    return std::nullopt;
}

std::optional<ModelInfo> AgentSession::resolve_model() const
{
    std::string model_id = settings_.model;
    // 自动探测/切换出的模型只在进程内有效，新建会话时沿用它。
    if (model_id.empty()) model_id = current_model_id_;
    if (model_id.empty()) return std::nullopt;
    if (const auto model = get_model(model_id)) return *model;
    return make_custom_model(model_id, settings_.baseUrl);
}

bool AgentSession::ensure_model(std::string* errorOut, std::string* noticeOut)
{
    const auto list = list_models();
    const std::string configured = harness_.model().id;

    if (!list.error.empty())
    {
        // 端点不可达（离线、代理、错误 baseUrl）：已有配置模型时先沿用，避免启动即失败
        if (!configured.empty()) return true;
        std::string reason = "GET /models 失败：" + list.error;
        // 端点返回 401/403 时最常见的根因是凭据缺失，直接给出可执行的提示。
        if (settings_.apiKey.empty())
        {
            reason += "（未配置 API key：PI_API_KEY / OPENAI_API_KEY）";
        }
        if (errorOut) *errorOut = reason;
        return false;
    }
    if (list.models.empty())
    {
        if (!configured.empty()) return true;
        if (errorOut) *errorOut = "端点没有返回任何模型（GET /models 结果为空）";
        return false;
    }

    // 配置的模型仍在端点列表里：保留（换端点后旧模型通常已不存在，见下）
    const bool configured_available =
        !configured.empty() &&
        std::any_of(list.models.begin(), list.models.end(),
                    [&](const RemoteModel& model) { return model.id == configured; });

    if (configured_available)
    {
        current_model_id_ = configured;
        return true;
    }

    // 没有配置，或配置的模型已不在端点（例如换了 baseUrl）：默认模型 = 端点里第一个
    // 看起来像对话模型的条目（端点常把语音模型排在前面），全都像非对话模型时退回第一个。
    // 只记在进程内（current_model_id_），不写 settings_.model：否则后续 /thinking 等
    // 写配置动作会把它一起持久化。只有用户主动 /model 选择才按 cwd 持久化。
    const auto pick =
        std::find_if(list.models.begin(), list.models.end(),
                     [](const RemoteModel& model) { return looks_like_chat_model(model.id); });
    const std::string model_id = pick != list.models.end() ? pick->id : list.models.front().id;
    auto model = get_model(model_id);
    if (!model) model = make_custom_model(model_id, settings_.baseUrl);

    harness_.set_model(*model);
    current_model_id_ = model_id;
    if (!configured.empty() && noticeOut)
    {
        *noticeOut = "配置的模型 " + configured + " 不在端点 " + settings_.baseUrl +
                     " 的模型列表中，已改用 " + model_id + "（用 /model 可切换）";
    }
    return true;
}

void AgentSession::new_session()
{
    const auto created = repo_.create(cwd_);
    if (!created.ok) throw std::runtime_error("Cannot create session: " + created.error.message);
    harness_.session() = created.value;
    // /new 新建会话文件后必须清空 agent 内存中的 transcript，否则下一轮 prompt
    // 会把旧会话的历史一并发送给 LLM，导致上下文与会话文件（/resume 恢复结果）脱节。
    harness_.reset();
    // 模型可能尚未确定（等 ensure_model 从 GET /models 取），此时沿用 harness 当前模型。
    if (const auto model = resolve_model()) harness_.set_model(*model);
    harness_.set_thinking_level(settings_.thinking);
    reset_session_usage();
}

void AgentSession::resume()
{
    const auto sessions = repo_.list(cwd_);
    if (sessions.ok && !sessions.value.empty())
    {
        std::string error;
        if (open_and_load(sessions.value[0], &error))
        {
            return;
        }
    }
    new_session();
}

bool AgentSession::resume_by_id(const std::string& id, std::string* errorOut)
{
    if (id.empty())
    {
        if (errorOut) *errorOut = "会话 id 不能为空";
        return false;
    }
    const auto sessions = repo_.list(cwd_);
    if (!sessions.ok || sessions.value.empty())
    {
        if (errorOut) *errorOut = "没有可恢复的会话（可用 /sessions 查看）";
        return false;
    }
    const SessionMetadata* target = nullptr;
    for (const auto& s : sessions.value)
    {
        if (s.id == id)
        {
            target = &s;
            break;
        }
    }
    if (!target)
    {
        // 支持 id 前缀匹配（uuid 前缀足够唯一即可）
        for (const auto& s : sessions.value)
        {
            if (s.id.compare(0, id.size(), id) == 0)
            {
                target = &s;
                break;
            }
        }
    }
    if (!target)
    {
        if (errorOut) *errorOut = "未找到会话 id：" + id;
        return false;
    }
    return open_and_load(*target, errorOut);
}

bool AgentSession::resume_by_index(int index, std::string* errorOut)
{
    const auto sessions = repo_.list(cwd_);
    if (!sessions.ok || index < 0 || index >= static_cast<int>(sessions.value.size()))
    {
        if (errorOut) *errorOut = "会话序号越界（可用 /sessions 查看）";
        return false;
    }
    return open_and_load(sessions.value[index], errorOut);
}

void AgentSession::prompt(const std::string& text, const std::vector<ContentBlock>& images)
{
    // 每轮开始清零"最近一次"用量：abort/失败后 UI 不应重复打印上一轮的 token 数。
    {
        std::lock_guard<std::mutex> lock(usage_mutex_);
        last_turn_usage_ = Usage{};
    }
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

void AgentSession::abort() { harness_.abort(); }

Usage AgentSession::session_usage() const
{
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return session_usage_;
}

Usage AgentSession::all_time_usage() const
{
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return all_time_usage_;
}

Usage AgentSession::last_turn_usage() const
{
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return last_turn_usage_;
}

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
    if (modelId.empty()) return;
    // 注册表里没有的 id（例如运行时从 GET /models 选中的远端模型）按自定义模型接入：
    // 沿用全局 baseUrl，缺省上下文窗口取 make_custom_model 的保守默认值。
    auto model = get_model(modelId);
    if (!model) model = make_custom_model(modelId, settings_.baseUrl);
    harness_.set_model(*model);
    settings_.model = modelId;
    current_model_id_ = modelId;
    // thinking 不做能力校验：真实请求失败时再把错误报给用户。
}

void AgentSession::set_thinking(ThinkingLevel level)
{
    harness_.set_thinking_level(level);
    settings_.thinking = level;
}

bool AgentSession::compact(std::string* errorOut) { return harness_.compact(errorOut); }

RemoteModelList AgentSession::list_models() const
{
    return list_remote_models(settings_.baseUrl, settings_.apiKey);
}

std::vector<SessionMetadata> AgentSession::list_sessions() const
{
    const auto sessions = repo_.list(cwd_);
    if (!sessions.ok) return {};
    return sessions.value;
}

std::string AgentSession::current_session_id() const { return harness_.session().metadata().id; }

std::vector<AgentMessage> AgentSession::messages() const { return harness_.messages(); }

ModelInfo AgentSession::model() const { return harness_.model(); }

ThinkingLevel AgentSession::thinking_level() const { return harness_.thinking_level(); }

std::string AgentSession::system_prompt() const { return harness_.system_prompt(); }

std::function<void()> AgentSession::subscribe(AgentEventListener listener)
{
    return harness_.subscribe(std::move(listener));
}

bool AgentSession::open_and_load(const SessionMetadata& metadata, std::string* errorOut)
{
    const auto opened = repo_.open(metadata);
    if (!opened.ok)
    {
        if (errorOut) *errorOut = opened.error.message;
        return false;
    }
    harness_.session() = opened.value;
    harness_.reload();
    reset_session_usage();
    return true;
}

void AgentSession::reset_session_usage()
{
    std::lock_guard<std::mutex> lock(usage_mutex_);
    session_usage_ = Usage{};
    last_turn_usage_ = Usage{};
}

}  // namespace pi

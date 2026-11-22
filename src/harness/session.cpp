#include "pi/harness/session.h"

#include <cstdio>
#include <ctime>

#include <set>
#include <sstream>

#include "pi/harness/uuid.h"

namespace pi
{

namespace
{

SessionError session_error(SessionErrorCode code, const std::string& message)
{
    SessionError error;
    error.code = code;
    error.message = message;
    return error;
}

}  // namespace

std::string iso_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t seconds = ms / 1000;
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return buf;
}

// ---------- 消息 ↔ wire JSON ----------

namespace
{

Json content_block_to_json(const ContentBlock& block)
{
    Json json = Json::object();
    switch (block.type)
    {
        case BlockType::Text:
            json["type"] = "text";
            json["text"] = block.text;
            break;
        case BlockType::Thinking:
            json["type"] = "thinking";
            json["thinking"] = block.thinking;
            if (!block.thinkingSignature.empty())
                json["thinkingSignature"] = block.thinkingSignature;
            if (block.redacted) json["redacted"] = true;
            break;
        case BlockType::Image:
            json["type"] = "image";
            json["data"] = block.data;
            json["mimeType"] = block.mimeType;
            break;
        case BlockType::ToolCall:
            json["type"] = "toolCall";
            json["id"] = block.id;
            json["name"] = block.name;
            json["arguments"] = block.arguments;
            if (!block.thoughtSignature.empty()) json["thoughtSignature"] = block.thoughtSignature;
            break;
    }
    return json;
}

ContentBlock content_block_from_json(const Json& json)
{
    ContentBlock block;
    const std::string type = json.value("type", "text");
    if (type == "text")
    {
        block.type = BlockType::Text;
        block.text = json.value("text", "");
    }
    else if (type == "thinking")
    {
        block.type = BlockType::Thinking;
        block.thinking = json.value("thinking", "");
        block.thinkingSignature = json.value("thinkingSignature", "");
        block.redacted = json.value("redacted", false);
    }
    else if (type == "image")
    {
        block.type = BlockType::Image;
        block.data = json.value("data", "");
        block.mimeType = json.value("mimeType", "");
    }
    else if (type == "toolCall")
    {
        block.type = BlockType::ToolCall;
        block.id = json.value("id", "");
        block.name = json.value("name", "");
        block.arguments = json.contains("arguments") ? json["arguments"] : Json::object();
        block.thoughtSignature = json.value("thoughtSignature", "");
    }
    return block;
}

}  // namespace

Json message_to_session_json(const Message& message)
{
    Json json = Json::object();
    json["role"] = to_string(message.role);
    Json content = Json::array();
    for (const auto& block : message.content) content.push_back(content_block_to_json(block));
    json["content"] = std::move(content);
    json["timestamp"] = message.timestamp;

    if (message.role == Role::Assistant)
    {
        if (!message.api.empty()) json["api"] = message.api;
        if (!message.provider.empty()) json["provider"] = message.provider;
        if (!message.model.empty()) json["model"] = message.model;
        if (!message.responseModel.empty()) json["responseModel"] = message.responseModel;
        if (!message.responseId.empty()) json["responseId"] = message.responseId;
        json["usage"] = Json{{"input", message.usage.input},
                             {"output", message.usage.output},
                             {"cacheRead", message.usage.cacheRead},
                             {"cacheWrite", message.usage.cacheWrite},
                             {"totalTokens", message.usage.totalTokens}};
        json["stopReason"] = to_string(message.stopReason);
        if (!message.errorMessage.empty()) json["errorMessage"] = message.errorMessage;
    }
    else if (message.role == Role::ToolResult)
    {
        json["toolCallId"] = message.toolCallId;
        json["toolName"] = message.toolName;
        json["isError"] = message.isError;
        if (message.details.is_object() && !message.details.empty())
            json["details"] = message.details;
    }
    return json;
}

Message message_from_session_json(const Json& json)
{
    Message message;
    const std::string role = json.value("role", "user");
    if (role == "assistant")
    {
        message.role = Role::Assistant;
    }
    else if (role == "toolResult")
    {
        message.role = Role::ToolResult;
    }
    else
    {
        message.role = Role::User;
    }
    if (json.contains("content") && json["content"].is_array())
    {
        for (const auto& block : json["content"])
            message.content.push_back(content_block_from_json(block));
    }
    message.timestamp = json.value("timestamp", 0LL);

    if (message.role == Role::Assistant)
    {
        message.api = json.value("api", "");
        message.provider = json.value("provider", "");
        message.model = json.value("model", "");
        message.responseModel = json.value("responseModel", "");
        message.responseId = json.value("responseId", "");
        if (json.contains("usage") && json["usage"].is_object())
        {
            const auto& usage = json["usage"];
            message.usage.input = usage.value("input", 0LL);
            message.usage.output = usage.value("output", 0LL);
            message.usage.cacheRead = usage.value("cacheRead", 0LL);
            message.usage.cacheWrite = usage.value("cacheWrite", 0LL);
            message.usage.totalTokens = usage.value("totalTokens", 0LL);
        }
        const std::string stop_reason = json.value("stopReason", "stop");
        message.stopReason = stop_reason == "stop"      ? StopReason::Stop
                             : stop_reason == "length"  ? StopReason::Length
                             : stop_reason == "toolUse" ? StopReason::ToolUse
                             : stop_reason == "error"   ? StopReason::Error
                                                        : StopReason::Aborted;
        message.errorMessage = json.value("errorMessage", "");
    }
    else if (message.role == Role::ToolResult)
    {
        message.toolCallId = json.value("toolCallId", "");
        message.toolName = json.value("toolName", "");
        message.isError = json.value("isError", false);
        if (json.contains("details")) message.details = json["details"];
    }
    return message;
}

// ---------- 条目 ↔ wire JSON ----------

Json session_entry_to_json(const SessionTreeEntry& entry)
{
    Json json = Json::object();
    const char* type = nullptr;
    switch (entry.type)
    {
        case SessionTreeEntry::Type::Message:
            type = "message";
            break;
        case SessionTreeEntry::Type::ThinkingLevelChange:
            type = "thinking_level_change";
            break;
        case SessionTreeEntry::Type::ModelChange:
            type = "model_change";
            break;
        case SessionTreeEntry::Type::ActiveToolsChange:
            type = "active_tools_change";
            break;
        case SessionTreeEntry::Type::Compaction:
            type = "compaction";
            break;
    }
    json["type"] = type;
    json["id"] = entry.id;
    json["parentId"] = entry.parentId.empty() ? Json(nullptr) : Json(entry.parentId);
    json["timestamp"] = entry.timestamp;

    switch (entry.type)
    {
        case SessionTreeEntry::Type::Message:
            json["message"] = message_to_session_json(entry.message);
            break;
        case SessionTreeEntry::Type::ThinkingLevelChange:
            json["thinkingLevel"] = entry.thinkingLevel;
            break;
        case SessionTreeEntry::Type::ModelChange:
            json["provider"] = entry.provider;
            json["modelId"] = entry.modelId;
            break;
        case SessionTreeEntry::Type::ActiveToolsChange:
            json["activeToolNames"] = entry.activeToolNames;
            break;
        case SessionTreeEntry::Type::Compaction:
            json["summary"] = entry.summary;
            json["firstKeptEntryId"] = entry.firstKeptEntryId;
            json["tokensBefore"] = entry.tokensBefore;
            if (entry.details.is_object() && !entry.details.empty())
                json["details"] = entry.details;
            if (entry.fromHook) json["fromHook"] = true;
            break;
    }
    return json;
}

SessionTreeEntry session_entry_from_json(const Json& json, const std::string& filePath,
                                         int lineNumber)
{
    auto fail = [&](const std::string& message) -> SessionError
    {
        return session_error(SessionErrorCode::InvalidEntry,
                             "Invalid JSONL session file " + filePath + ": line " +
                                 std::to_string(lineNumber) + " " + message);
    };

    if (!json.is_object()) throw fail("is not a valid session entry");
    const std::string type = json.value("type", "");
    const std::string id = json.value("id", "");
    if (id.empty()) throw fail("is missing entry id");
    const std::string timestamp = json.value("timestamp", "");
    if (timestamp.empty()) throw fail("is missing timestamp");

    SessionTreeEntry entry;
    entry.id = id;
    entry.timestamp = timestamp;
    if (json.contains("parentId") && !json["parentId"].is_null())
    {
        if (!json["parentId"].is_string()) throw fail("has invalid parentId");
        entry.parentId = json["parentId"].get<std::string>();
    }

    if (type == "message")
    {
        entry.type = SessionTreeEntry::Type::Message;
        if (!json.contains("message")) throw fail("is missing message");
        entry.message = message_from_session_json(json["message"]);
    }
    else if (type == "thinking_level_change")
    {
        entry.type = SessionTreeEntry::Type::ThinkingLevelChange;
        entry.thinkingLevel = json.value("thinkingLevel", "");
    }
    else if (type == "model_change")
    {
        entry.type = SessionTreeEntry::Type::ModelChange;
        entry.provider = json.value("provider", "");
        entry.modelId = json.value("modelId", "");
    }
    else if (type == "active_tools_change")
    {
        entry.type = SessionTreeEntry::Type::ActiveToolsChange;
        if (json.contains("activeToolNames") && json["activeToolNames"].is_array())
        {
            for (const auto& name : json["activeToolNames"])
                entry.activeToolNames.push_back(name.get<std::string>());
        }
    }
    else if (type == "compaction")
    {
        entry.type = SessionTreeEntry::Type::Compaction;
        entry.summary = json.value("summary", "");
        entry.firstKeptEntryId = json.value("firstKeptEntryId", "");
        entry.tokensBefore = json.value("tokensBefore", 0LL);
        if (json.contains("details")) entry.details = json["details"];
        entry.fromHook = json.value("fromHook", false);
    }
    else
    {
        throw fail("is missing entry type");
    }
    return entry;
}

// ---------- JsonlSessionStorage ----------

namespace
{

std::string generate_entry_id(const std::map<std::string, size_t>& by_id)
{
    for (int i = 0; i < 100; ++i)
    {
        const std::string candidate = uuidv7().substr(0, 8);
        if (by_id.find(candidate) == by_id.end()) return candidate;
    }
    return uuidv7();
}

}  // namespace

JsonlSessionStorage::JsonlSessionStorage(JsonlSessionStorage&& other) noexcept
    : fs_(other.fs_),
      file_path_(std::move(other.file_path_)),
      metadata_(std::move(other.metadata_)),
      entries_(std::move(other.entries_)),
      by_id_(std::move(other.by_id_)),
      current_leaf_id_(std::move(other.current_leaf_id_))
{
    // mutex_ 不随对象移动：新对象持有一把全新的锁。
    // 移动只发生在存储被共享（shared_ptr）之前，因此不涉及并发持锁对象搬迁。
}

JsonlSessionStorage& JsonlSessionStorage::operator=(JsonlSessionStorage&& other) noexcept
{
    if (this != &other)
    {
        fs_ = other.fs_;
        file_path_ = std::move(other.file_path_);
        metadata_ = std::move(other.metadata_);
        entries_ = std::move(other.entries_);
        by_id_ = std::move(other.by_id_);
        current_leaf_id_ = std::move(other.current_leaf_id_);
    }
    return *this;
}

Result<JsonlSessionStorage, SessionError> JsonlSessionStorage::open(FileSystem& fs,
                                                                    const std::string& path)
{
    const auto content = fs.read_text_file(path);
    if (!content.ok)
    {
        return Result<JsonlSessionStorage, SessionError>::err_value(
            session_error(SessionErrorCode::InvalidSession,
                          "Failed to read session " + path + ": " + content.error.message));
    }
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(content.value);
    while (std::getline(stream, line))
    {
        if (line.find_first_not_of(" \t\r\n") != std::string::npos) lines.push_back(line);
    }
    if (lines.empty())
    {
        return Result<JsonlSessionStorage, SessionError>::err_value(
            session_error(SessionErrorCode::InvalidSession,
                          "Invalid JSONL session file " + path + ": missing session header"));
    }

    // header
    Json header;
    try
    {
        header = Json::parse(lines[0]);
    }
    catch (...)
    {
        return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
            SessionErrorCode::InvalidSession,
            "Invalid JSONL session file " + path + ": first line is not a valid session header"));
    }
    // 字段类型错误（如 "version":"3"）会抛 nlohmann 异常：统一转成 SessionError，
    // 否则坏会话文件会把异常抛到 main，直接终止进程。
    bool supported_header = false;
    try
    {
        supported_header = header.is_object() && header.value("type", "") == "session" &&
                           header.value("version", 0) == 3;
    }
    catch (const std::exception&)
    {
        supported_header = false;
    }
    if (!supported_header)
    {
        return Result<JsonlSessionStorage, SessionError>::err_value(
            session_error(SessionErrorCode::InvalidSession,
                          "Invalid JSONL session file " + path + ": unsupported session header"));
    }

    JsonlSessionStorage storage;
    storage.fs_ = &fs;
    storage.file_path_ = path;
    storage.metadata_.id = header.value("id", "");
    storage.metadata_.createdAt = header.value("timestamp", "");
    storage.metadata_.cwd = header.value("cwd", "");
    storage.metadata_.path = path;

    // 已解析成功的部分（用于尾行损坏时的截断修复）
    std::string valid_prefix = lines[0] + "\n";
    for (size_t i = 1; i < lines.size(); ++i)
    {
        Json json;
        try
        {
            json = Json::parse(lines[i]);
        }
        catch (...)
        {
            // 尾行损坏容忍：最后一条非空行不是合法 JSON，多为进程在追加中途被
            // 强杀（Ctrl+C/SIGKILL）留下的半行。截断到最后一个完整行并写回，
            // 保证会话可恢复打开；文件中部的损坏仍然拒绝（那才是真正的数据问题）。
            if (i + 1 == lines.size())
            {
                const auto repaired = fs.write_file(path, valid_prefix);
                if (!repaired.ok)
                {
                    return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
                        SessionErrorCode::Storage, "Failed to repair truncated session " + path +
                                                       ": " + repaired.error.message));
                }
                break;
            }
            return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
                SessionErrorCode::InvalidEntry, "Invalid JSONL session file " + path + ": line " +
                                                    std::to_string(i + 1) + " is not valid JSON"));
        }
        SessionTreeEntry entry;
        try
        {
            entry = session_entry_from_json(json, path, static_cast<int>(i + 1));
        }
        catch (const SessionError& error)
        {
            return Result<JsonlSessionStorage, SessionError>::err_value(error);
        }
        catch (const std::exception& error)
        {
            // 合法 JSON 但字段类型不符：同样视为损坏条目，跳过/拒绝而不是崩溃
            return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
                SessionErrorCode::InvalidEntry, "Invalid JSONL session file " + path + ": line " +
                                                    std::to_string(i + 1) + " " + error.what()));
        }
        if (storage.by_id_.find(entry.id) != storage.by_id_.end())
        {
            return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
                SessionErrorCode::InvalidEntry,
                "Invalid JSONL session file " + path + ": duplicate entry id " + entry.id));
        }
        if (!entry.parentId.empty() && storage.by_id_.find(entry.parentId) == storage.by_id_.end())
        {
            return Result<JsonlSessionStorage, SessionError>::err_value(session_error(
                SessionErrorCode::InvalidEntry, "Invalid JSONL session file " + path + ": line " +
                                                    std::to_string(i + 1) +
                                                    " references an unknown parent"));
        }
        valid_prefix += lines[i] + "\n";
        storage.entries_.push_back(entry);
        storage.by_id_[entry.id] = storage.entries_.size() - 1;
        storage.current_leaf_id_ = entry.id;
    }

    return Result<JsonlSessionStorage, SessionError>::ok_value(std::move(storage));
}

Result<JsonlSessionStorage, SessionError> JsonlSessionStorage::create(FileSystem& fs,
                                                                      const std::string& path,
                                                                      const std::string& cwd,
                                                                      const std::string& sessionId)
{
    Json header = Json::object();
    header["type"] = "session";
    header["version"] = 3;
    header["id"] = sessionId;
    header["timestamp"] = iso_timestamp();
    header["cwd"] = cwd;

    const auto write = fs.write_file(path, header.dump() + "\n");
    if (!write.ok)
    {
        return Result<JsonlSessionStorage, SessionError>::err_value(
            session_error(SessionErrorCode::Storage,
                          "Failed to create session " + path + ": " + write.error.message));
    }

    JsonlSessionStorage storage;
    storage.fs_ = &fs;
    storage.file_path_ = path;
    storage.metadata_.id = sessionId;
    storage.metadata_.createdAt = header["timestamp"].get<std::string>();
    storage.metadata_.cwd = cwd;
    storage.metadata_.path = path;
    return Result<JsonlSessionStorage, SessionError>::ok_value(std::move(storage));
}

Result<std::string, SessionError> JsonlSessionStorage::get_leaf_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_leaf_id_.empty() && by_id_.find(current_leaf_id_) == by_id_.end())
    {
        return Result<std::string, SessionError>::err_value(session_error(
            SessionErrorCode::InvalidSession, "Entry " + current_leaf_id_ + " not found"));
    }
    return Result<std::string, SessionError>::ok_value(current_leaf_id_);
}

Result<std::string, SessionError> JsonlSessionStorage::append_child(SessionTreeEntry entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    entry.id = generate_entry_id(by_id_);
    entry.parentId = current_leaf_id_;
    entry.timestamp = iso_timestamp();
    const auto append = append_entry_locked(std::move(entry));
    if (!append.ok) return Result<std::string, SessionError>::err_value(append.error);
    return Result<std::string, SessionError>::ok_value(entries_.back().id);
}

Result<void, SessionError> JsonlSessionStorage::append_entry_locked(SessionTreeEntry entry)
{
    if (entry.id.empty() || by_id_.find(entry.id) != by_id_.end())
    {
        return Result<void, SessionError>::err_value(session_error(
            SessionErrorCode::InvalidEntry, "Session entry id is missing or duplicated"));
    }
    if (!entry.parentId.empty() && by_id_.find(entry.parentId) == by_id_.end())
    {
        return Result<void, SessionError>::err_value(session_error(
            SessionErrorCode::NotFound, "Parent entry " + entry.parentId + " not found"));
    }
    const auto append = fs_->append_file(file_path_, session_entry_to_json(entry).dump() + "\n");
    if (!append.ok)
    {
        return Result<void, SessionError>::err_value(session_error(
            SessionErrorCode::Storage, "Failed to append session entry: " + append.error.message));
    }
    by_id_[entry.id] = entries_.size();
    entries_.push_back(std::move(entry));
    current_leaf_id_ = entries_.back().id;
    return Result<void, SessionError>::ok_value();
}

Result<std::vector<SessionTreeEntry>, SessionError> JsonlSessionStorage::get_path_to_root(
    const std::string& leafId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionTreeEntry> path;
    if (leafId.empty()) return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(path);
    std::string current = leafId;
    std::set<std::string> visited;
    while (true)
    {
        if (!visited.insert(current).second)
        {
            return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
                session_error(SessionErrorCode::InvalidSession,
                              "Session tree contains a parent cycle at entry " + current));
        }
        const auto it = by_id_.find(current);
        if (it == by_id_.end())
        {
            return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
                session_error(SessionErrorCode::NotFound, "Entry " + current + " not found"));
        }
        path.push_back(entries_[it->second]);
        const auto& entry = entries_[it->second];
        if (entry.parentId.empty()) break;
        current = entry.parentId;
    }
    std::reverse(path.begin(), path.end());
    return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(std::move(path));
}

Result<std::vector<SessionTreeEntry>, SessionError> JsonlSessionStorage::get_entries() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(entries_);
}

// ---------- Session ----------

SessionMetadata Session::metadata() const
{
    return storage_ ? storage_->metadata() : SessionMetadata{};
}

Result<std::vector<SessionTreeEntry>, SessionError> Session::get_entries() const
{
    if (!storage_)
        return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    return storage_->get_entries();
}

Result<std::vector<SessionTreeEntry>, SessionError> Session::get_branch(
    const std::string& fromId) const
{
    if (!storage_)
        return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    if (!fromId.empty()) return storage_->get_path_to_root(fromId);
    const auto leaf = storage_->get_leaf_id();
    if (!leaf.ok) return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(leaf.error);
    return storage_->get_path_to_root(leaf.value);
}

Result<SessionContext, SessionError> Session::build_context() const
{
    const auto branch = get_branch();
    if (!branch.ok) return Result<SessionContext, SessionError>::err_value(branch.error);

    SessionContext context;
    std::string thinking_level = "off";
    std::optional<std::pair<std::string, std::string>> model;
    std::optional<std::vector<std::string>> active_tools;
    const SessionTreeEntry* compaction = nullptr;

    for (const auto& entry : branch.value)
    {
        switch (entry.type)
        {
            case SessionTreeEntry::Type::ThinkingLevelChange:
                thinking_level = entry.thinkingLevel;
                break;
            case SessionTreeEntry::Type::ModelChange:
                model = std::make_pair(entry.provider, entry.modelId);
                break;
            case SessionTreeEntry::Type::Message:
                if (entry.message.role == Role::Assistant)
                {
                    model = std::make_pair(entry.message.provider, entry.message.model);
                }
                break;
            case SessionTreeEntry::Type::ActiveToolsChange:
                active_tools = entry.activeToolNames;
                break;
            case SessionTreeEntry::Type::Compaction:
                compaction = &entry;
                break;
            default:
                break;
        }
    }
    context.thinkingLevel = thinking_level;
    context.model = model;
    context.activeToolNames = active_tools;

    std::vector<AgentMessage> messages;
    const auto append_message = [&](const SessionTreeEntry& entry)
    {
        if (entry.type == SessionTreeEntry::Type::Message)
        {
            messages.push_back(entry.message);
        }
    };

    if (compaction)
    {
        Message summary;
        summary.role = Role::User;
        summary.content.push_back(ContentBlock{});
        summary.content.back().type = BlockType::Text;
        summary.content.back().text =
            "The conversation history before this point was compacted into the following "
            "summary:\n\n<summary>\n" +
            compaction->summary + "\n</summary>";
        messages.push_back(std::move(summary));

        bool found_first_kept = false;
        for (const auto& entry : branch.value)
        {
            if (entry.id == compaction->firstKeptEntryId) found_first_kept = true;
            if (found_first_kept) append_message(entry);
        }
    }
    else
    {
        for (const auto& entry : branch.value) append_message(entry);
    }

    context.messages = std::move(messages);
    return Result<SessionContext, SessionError>::ok_value(std::move(context));
}

Result<std::string, SessionError> Session::append_typed_entry(SessionTreeEntry entry)
{
    if (!storage_)
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    // append_child 在存储内部一次加锁完成 id 分配、parent 挂接与落盘，
    // 保证 UI 线程（切换命令）与 run 线程（消息持久化）并发追加时历史链保持线性。
    return storage_->append_child(std::move(entry));
}

Result<std::string, SessionError> Session::append_message(const AgentMessage& message)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::Message;
    entry.message = message;
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_thinking_level_change(const std::string& level)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::ThinkingLevelChange;
    entry.thinkingLevel = level;
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_model_change(const std::string& provider,
                                                               const std::string& modelId)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::ModelChange;
    entry.provider = provider;
    entry.modelId = modelId;
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_active_tools_change(
    const std::vector<std::string>& names)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::ActiveToolsChange;
    entry.activeToolNames = names;
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_compaction(const std::string& summary,
                                                             const std::string& firstKeptEntryId,
                                                             int64_t tokensBefore,
                                                             const Json& details, bool fromHook)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::Compaction;
    entry.summary = summary;
    entry.firstKeptEntryId = firstKeptEntryId;
    entry.tokensBefore = tokensBefore;
    entry.details = details;
    entry.fromHook = fromHook;
    return append_typed_entry(std::move(entry));
}

}  // namespace pi

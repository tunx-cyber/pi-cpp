#include "pi/harness/session.h"

#include <cstdio>
#include <ctime>

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
                             {"totalTokens", message.usage.totalTokens},
                             {"cost", Json{{"input", message.usage.cost.input},
                                           {"output", message.usage.cost.output},
                                           {"cacheRead", message.usage.cost.cacheRead},
                                           {"cacheWrite", message.usage.cost.cacheWrite},
                                           {"total", message.usage.cost.total}}}};
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
        case SessionTreeEntry::Type::BranchSummary:
            type = "branch_summary";
            break;
        case SessionTreeEntry::Type::Custom:
            type = "custom";
            break;
        case SessionTreeEntry::Type::CustomMessage:
            type = "custom_message";
            break;
        case SessionTreeEntry::Type::Label:
            type = "label";
            break;
        case SessionTreeEntry::Type::SessionInfo:
            type = "session_info";
            break;
        case SessionTreeEntry::Type::Leaf:
            type = "leaf";
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
        case SessionTreeEntry::Type::BranchSummary:
            json["fromId"] = entry.fromId;
            json["summary"] = entry.summary;
            if (entry.details.is_object() && !entry.details.empty())
                json["details"] = entry.details;
            if (entry.fromHook) json["fromHook"] = true;
            break;
        case SessionTreeEntry::Type::Custom:
            json["customType"] = entry.customType;
            if (entry.details.is_object() && !entry.details.empty()) json["data"] = entry.details;
            break;
        case SessionTreeEntry::Type::CustomMessage:
            json["customType"] = entry.customType;
            json["content"] = entry.message.role == Role::User && entry.message.content.size() == 1
                                  ? Json(entry.message.text_content())
                                  : Json::array();
            json["display"] = entry.display;
            break;
        case SessionTreeEntry::Type::Label:
            json["targetId"] = entry.targetId;
            json["label"] = entry.label.empty() ? Json(nullptr) : Json(entry.label);
            break;
        case SessionTreeEntry::Type::SessionInfo:
            if (!entry.name.empty()) json["name"] = entry.name;
            break;
        case SessionTreeEntry::Type::Leaf:
            json["targetId"] =
                entry.leafTargetId.empty() ? Json(nullptr) : Json(entry.leafTargetId);
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
    else if (type == "branch_summary")
    {
        entry.type = SessionTreeEntry::Type::BranchSummary;
        entry.fromId = json.value("fromId", "");
        entry.summary = json.value("summary", "");
        if (json.contains("details")) entry.details = json["details"];
        entry.fromHook = json.value("fromHook", false);
    }
    else if (type == "custom")
    {
        entry.type = SessionTreeEntry::Type::Custom;
        entry.customType = json.value("customType", "");
        if (json.contains("data")) entry.details = json["data"];
    }
    else if (type == "custom_message")
    {
        entry.type = SessionTreeEntry::Type::CustomMessage;
        entry.customType = json.value("customType", "");
        entry.display = json.value("display", false);
    }
    else if (type == "label")
    {
        entry.type = SessionTreeEntry::Type::Label;
        entry.targetId = json.value("targetId", "");
        if (json.contains("label") && !json["label"].is_null())
            entry.label = json["label"].get<std::string>();
    }
    else if (type == "session_info")
    {
        entry.type = SessionTreeEntry::Type::SessionInfo;
        entry.name = json.value("name", "");
    }
    else if (type == "leaf")
    {
        entry.type = SessionTreeEntry::Type::Leaf;
        if (json.contains("targetId") && !json["targetId"].is_null())
        {
            entry.leafTargetId = json["targetId"].get<std::string>();
        }
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
    if (header.value("type", "") != "session" || header.value("version", 0) != 3)
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
    storage.metadata_.parentSessionPath = header.value("parentSession", "");

    for (size_t i = 1; i < lines.size(); ++i)
    {
        Json json;
        try
        {
            json = Json::parse(lines[i]);
        }
        catch (...)
        {
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
        storage.entries_.push_back(entry);
        storage.by_id_[entry.id] = storage.entries_.size() - 1;
        if (entry.type == SessionTreeEntry::Type::Label && !entry.label.empty())
        {
            storage.labels_by_id_[entry.targetId] = entry.label;
        }
        else if (entry.type == SessionTreeEntry::Type::Label)
        {
            storage.labels_by_id_.erase(entry.targetId);
        }
        if (entry.type == SessionTreeEntry::Type::Leaf)
        {
            storage.current_leaf_id_ = entry.leafTargetId;
        }
        else
        {
            storage.current_leaf_id_ = entry.id;
        }
    }

    return Result<JsonlSessionStorage, SessionError>::ok_value(std::move(storage));
}

Result<JsonlSessionStorage, SessionError> JsonlSessionStorage::create(
    FileSystem& fs, const std::string& path, const std::string& cwd, const std::string& sessionId,
    const std::string& parentSessionPath)
{
    Json header = Json::object();
    header["type"] = "session";
    header["version"] = 3;
    header["id"] = sessionId;
    header["timestamp"] = iso_timestamp();
    header["cwd"] = cwd;
    if (!parentSessionPath.empty()) header["parentSession"] = parentSessionPath;

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
    storage.metadata_.parentSessionPath = parentSessionPath;
    return Result<JsonlSessionStorage, SessionError>::ok_value(std::move(storage));
}

Result<std::string, SessionError> JsonlSessionStorage::get_leaf_id() const
{
    if (!current_leaf_id_.empty() && by_id_.find(current_leaf_id_) == by_id_.end())
    {
        return Result<std::string, SessionError>::err_value(session_error(
            SessionErrorCode::InvalidSession, "Entry " + current_leaf_id_ + " not found"));
    }
    return Result<std::string, SessionError>::ok_value(current_leaf_id_);
}

Result<void, SessionError> JsonlSessionStorage::set_leaf_id(const std::string& leafId)
{
    if (!leafId.empty() && by_id_.find(leafId) == by_id_.end())
    {
        return Result<void, SessionError>::err_value(
            session_error(SessionErrorCode::NotFound, "Entry " + leafId + " not found"));
    }
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::Leaf;
    entry.id = generate_entry_id(by_id_);
    entry.parentId = current_leaf_id_;
    entry.timestamp = iso_timestamp();
    entry.leafTargetId = leafId;
    const auto append = append_entry(entry);
    if (!append.ok) return Result<void, SessionError>::err_value(append.error);
    return Result<void, SessionError>::ok_value();
}

std::string JsonlSessionStorage::create_entry_id() { return generate_entry_id(by_id_); }

Result<void, SessionError> JsonlSessionStorage::append_entry(SessionTreeEntry entry)
{
    const auto append = fs_->append_file(file_path_, session_entry_to_json(entry).dump() + "\n");
    if (!append.ok)
    {
        return Result<void, SessionError>::err_value(session_error(
            SessionErrorCode::Storage, "Failed to append session entry: " + append.error.message));
    }
    by_id_[entry.id] = entries_.size();
    entries_.push_back(std::move(entry));
    const auto& pushed = entries_.back();
    if (pushed.type == SessionTreeEntry::Type::Label && !pushed.label.empty())
    {
        labels_by_id_[pushed.targetId] = pushed.label;
    }
    else if (pushed.type == SessionTreeEntry::Type::Label)
    {
        labels_by_id_.erase(pushed.targetId);
    }
    if (pushed.type == SessionTreeEntry::Type::Leaf)
    {
        current_leaf_id_ = pushed.leafTargetId;
    }
    else
    {
        current_leaf_id_ = pushed.id;
    }
    return Result<void, SessionError>::ok_value();
}

Result<std::optional<SessionTreeEntry>, SessionError> JsonlSessionStorage::get_entry(
    const std::string& id) const
{
    const auto it = by_id_.find(id);
    if (it == by_id_.end())
        return Result<std::optional<SessionTreeEntry>, SessionError>::ok_value(std::nullopt);
    return Result<std::optional<SessionTreeEntry>, SessionError>::ok_value(entries_[it->second]);
}

Result<std::vector<SessionTreeEntry>, SessionError> JsonlSessionStorage::find_entries(
    SessionTreeEntry::Type type) const
{
    std::vector<SessionTreeEntry> found;
    for (const auto& entry : entries_)
    {
        if (entry.type == type) found.push_back(entry);
    }
    return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(std::move(found));
}

Result<std::optional<std::string>, SessionError> JsonlSessionStorage::get_label(
    const std::string& id) const
{
    const auto it = labels_by_id_.find(id);
    if (it == labels_by_id_.end())
        return Result<std::optional<std::string>, SessionError>::ok_value(std::nullopt);
    return Result<std::optional<std::string>, SessionError>::ok_value(it->second);
}

Result<std::vector<SessionTreeEntry>, SessionError> JsonlSessionStorage::get_path_to_root(
    const std::string& leafId) const
{
    std::vector<SessionTreeEntry> path;
    if (leafId.empty()) return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(path);
    std::string current = leafId;
    while (true)
    {
        const auto it = by_id_.find(current);
        if (it == by_id_.end())
        {
            return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
                session_error(SessionErrorCode::NotFound, "Entry " + current + " not found"));
        }
        path.insert(path.begin(), entries_[it->second]);
        const auto& entry = entries_[it->second];
        if (entry.parentId.empty()) break;
        current = entry.parentId;
    }
    return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(std::move(path));
}

Result<std::vector<SessionTreeEntry>, SessionError> JsonlSessionStorage::get_entries() const
{
    return Result<std::vector<SessionTreeEntry>, SessionError>::ok_value(entries_);
}

// ---------- Session ----------

SessionMetadata Session::metadata() const
{
    return storage_ ? storage_->metadata() : SessionMetadata{};
}

Result<std::string, SessionError> Session::get_leaf_id() const
{
    if (!storage_)
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    return storage_->get_leaf_id();
}

Result<std::vector<SessionTreeEntry>, SessionError> Session::get_entries() const
{
    if (!storage_)
        return Result<std::vector<SessionTreeEntry>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    return storage_->get_entries();
}

Result<std::optional<SessionTreeEntry>, SessionError> Session::get_entry(
    const std::string& id) const
{
    if (!storage_)
        return Result<std::optional<SessionTreeEntry>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    return storage_->get_entry(id);
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
        else if (entry.type == SessionTreeEntry::Type::BranchSummary)
        {
            Message summary;
            summary.role = Role::User;
            summary.timestamp = 0;
            summary.content.push_back(ContentBlock{});
            summary.content.back().type = BlockType::Text;
            summary.content.back().text =
                "The following is a summary of a branch that this conversation came back "
                "from:\n\n<summary>\n" +
                entry.summary + "</summary>";
            messages.push_back(std::move(summary));
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

Result<std::optional<std::string>, SessionError> Session::get_label(const std::string& id) const
{
    if (!storage_)
        return Result<std::optional<std::string>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    return storage_->get_label(id);
}

Result<std::optional<std::string>, SessionError> Session::get_session_name() const
{
    if (!storage_)
        return Result<std::optional<std::string>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    const auto entries = storage_->find_entries(SessionTreeEntry::Type::SessionInfo);
    if (!entries.ok)
        return Result<std::optional<std::string>, SessionError>::err_value(entries.error);
    if (entries.value.empty())
        return Result<std::optional<std::string>, SessionError>::ok_value(std::nullopt);
    const auto& name = entries.value.back().name;
    std::string trimmed = name;
    // trim
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (start == std::string::npos) ? "" : trimmed.substr(start, end - start + 1);
    if (trimmed.empty())
        return Result<std::optional<std::string>, SessionError>::ok_value(std::nullopt);
    return Result<std::optional<std::string>, SessionError>::ok_value(trimmed);
}

Result<std::string, SessionError> Session::append_typed_entry(SessionTreeEntry entry)
{
    if (!storage_)
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    entry.id = storage_->create_entry_id();
    const auto leaf = storage_->get_leaf_id();
    if (!leaf.ok) return Result<std::string, SessionError>::err_value(leaf.error);
    entry.parentId = leaf.value;
    entry.timestamp = iso_timestamp();
    const auto append = storage_->append_entry(std::move(entry));
    if (!append.ok) return Result<std::string, SessionError>::err_value(append.error);
    const auto& entries = storage_->get_entries();
    return Result<std::string, SessionError>::ok_value(
        entries.value.empty() ? "" : entries.value.back().id);
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

Result<std::string, SessionError> Session::append_custom_entry(const std::string& customType,
                                                               const Json& data)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::Custom;
    entry.customType = customType;
    entry.details = data;
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_custom_message_entry(
    const std::string& customType, const std::string& content, bool display)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::CustomMessage;
    entry.customType = customType;
    entry.display = display;
    Message message;
    message.role = Role::User;
    message.content.push_back(ContentBlock{});
    message.content.back().type = BlockType::Text;
    message.content.back().text = content;
    entry.message = std::move(message);
    return append_typed_entry(std::move(entry));
}

Result<std::string, SessionError> Session::append_label(const std::string& targetId,
                                                        const std::string& label)
{
    if (!storage_)
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    const auto entry = storage_->get_entry(targetId);
    if (!entry.ok) return Result<std::string, SessionError>::err_value(entry.error);
    if (!entry.value.has_value())
    {
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::NotFound, "Entry " + targetId + " not found"));
    }
    SessionTreeEntry label_entry;
    label_entry.type = SessionTreeEntry::Type::Label;
    label_entry.targetId = targetId;
    label_entry.label = label;
    return append_typed_entry(std::move(label_entry));
}

Result<std::string, SessionError> Session::append_session_name(const std::string& name)
{
    SessionTreeEntry entry;
    entry.type = SessionTreeEntry::Type::SessionInfo;
    entry.name = name;
    return append_typed_entry(std::move(entry));
}

Result<std::optional<std::string>, SessionError> Session::move_to(
    const std::string& entryId, const std::optional<std::string>& summary)
{
    if (!storage_)
        return Result<std::optional<std::string>, SessionError>::err_value(
            session_error(SessionErrorCode::Unknown, "no storage"));
    if (!entryId.empty())
    {
        const auto entry = storage_->get_entry(entryId);
        if (!entry.ok)
            return Result<std::optional<std::string>, SessionError>::err_value(entry.error);
        if (!entry.value.has_value())
        {
            return Result<std::optional<std::string>, SessionError>::err_value(
                session_error(SessionErrorCode::NotFound, "Entry " + entryId + " not found"));
        }
    }
    const auto set = storage_->set_leaf_id(entryId);
    if (!set.ok) return Result<std::optional<std::string>, SessionError>::err_value(set.error);
    if (!summary) return Result<std::optional<std::string>, SessionError>::ok_value(std::nullopt);
    SessionTreeEntry branch_summary;
    branch_summary.type = SessionTreeEntry::Type::BranchSummary;
    branch_summary.fromId = entryId.empty() ? "root" : entryId;
    branch_summary.parentId = entryId;
    branch_summary.summary = *summary;
    const auto id = append_typed_entry(std::move(branch_summary));
    if (!id.ok) return Result<std::optional<std::string>, SessionError>::err_value(id.error);
    return Result<std::optional<std::string>, SessionError>::ok_value(id.value);
}

}  // namespace pi

#include "pi/harness/jsonl_repo.h"

#include <algorithm>

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

std::string encode_cwd(const std::string& cwd)
{
    std::string encoded = cwd;
    if (!encoded.empty() && (encoded.front() == '/' || encoded.front() == '\\'))
        encoded.erase(encoded.begin());
    std::string out = "--";
    for (char c : encoded)
    {
        if (c == '/' || c == '\\' || c == ':')
        {
            out += '-';
        }
        else
        {
            out += c;
        }
    }
    out += "--";
    return out;
}

/** 将首条用户消息折叠为单行预览（换行→空格、压缩空白、UTF-8 边界截断）。 */
/** 取条目顶层的 type 字段（nlohmann 对象按 key 排序，type 总在行末，故取最后一个）。 */
std::string entry_type_of(const std::string& line)
{
    const auto pos = line.rfind("\"type\":\"");
    if (pos == std::string::npos) return "";
    const auto start = pos + 8;  // strlen(""type":"")
    const auto end = line.find('"', start);
    if (end == std::string::npos) return "";
    return line.substr(start, end - start);
}

std::string one_line_preview(const std::string& text)
{
    std::string collapsed;
    bool prev_space = true;  // 起始按"前导空白"处理，跳过前导空格
    for (char c : text)
    {
        const char ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (ch == ' ')
        {
            if (prev_space) continue;
            prev_space = true;
        }
        else
        {
            prev_space = false;
        }
        collapsed += ch;
    }
    // 去尾部空白
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();

    constexpr size_t kMaxPreview = 80;
    if (collapsed.size() <= kMaxPreview) return collapsed;
    size_t end = kMaxPreview;
    // 回退到 UTF-8 字符边界，避免切断多字节字符
    while (end > 0 && (static_cast<unsigned char>(collapsed[end]) & 0xC0) == 0x80) --end;
    return collapsed.substr(0, end) + "…";
}

}  // namespace

JsonlSessionRepo::JsonlSessionRepo(FileSystem& fs, std::string sessionsRoot)
    : fs_(fs), sessions_root_(std::move(sessionsRoot))
{
}

Result<std::string, SessionError> JsonlSessionRepo::get_session_dir(const std::string& cwd) const
{
    if (!root_resolved_)
    {
        const auto resolved = fs_.absolute_path(sessions_root_);
        if (!resolved.ok)
        {
            return Result<std::string, SessionError>::err_value(
                session_error(SessionErrorCode::Storage,
                              "Failed to resolve sessions root: " + resolved.error.message));
        }
        sessions_root_abs_ = resolved.value;
        root_resolved_ = true;
    }
    const auto joined = fs_.join_path({sessions_root_abs_, encode_cwd(cwd)});
    if (!joined.ok)
    {
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Storage,
                          "Failed to resolve session directory: " + joined.error.message));
    }
    return Result<std::string, SessionError>::ok_value(joined.value);
}

Result<std::string, SessionError> JsonlSessionRepo::create_session_file_path(
    const std::string& cwd, const std::string& sessionId, const std::string& timestamp) const
{
    const auto dir = get_session_dir(cwd);
    if (!dir.ok) return dir;
    std::string safe_ts = timestamp;
    for (char& c : safe_ts)
    {
        if (c == ':' || c == '.') c = '-';
    }
    const auto joined = fs_.join_path({dir.value, safe_ts + "_" + sessionId + ".jsonl"});
    if (!joined.ok)
    {
        return Result<std::string, SessionError>::err_value(
            session_error(SessionErrorCode::Storage,
                          "Failed to resolve session file path: " + joined.error.message));
    }
    return Result<std::string, SessionError>::ok_value(joined.value);
}

Result<Session, SessionError> JsonlSessionRepo::create(const std::string& cwd)
{
    const std::string id = uuidv7();
    const std::string createdAt = iso_timestamp();
    const auto dir = get_session_dir(cwd);
    if (!dir.ok) return Result<Session, SessionError>::err_value(dir.error);
    const auto mkdir = fs_.create_dir(dir.value, true);
    if (!mkdir.ok)
    {
        return Result<Session, SessionError>::err_value(
            session_error(SessionErrorCode::Storage,
                          "Failed to create session directory: " + mkdir.error.message));
    }
    const auto file_path = create_session_file_path(cwd, id, createdAt);
    if (!file_path.ok) return Result<Session, SessionError>::err_value(file_path.error);
    // 注意：storage 不能声明为 const，否则 std::move 得到 const 右值，
    // 无法绑定移动构造（拷贝构造已被禁用）。
    auto storage = JsonlSessionStorage::create(fs_, file_path.value, cwd, id);
    if (!storage.ok) return Result<Session, SessionError>::err_value(storage.error);
    return Result<Session, SessionError>::ok_value(
        Session(std::make_shared<JsonlSessionStorage>(std::move(storage.value))));
}

Result<Session, SessionError> JsonlSessionRepo::open(const SessionMetadata& metadata)
{
    const auto exists = fs_.exists(metadata.path);
    if (!exists.ok)
    {
        return Result<Session, SessionError>::err_value(session_error(
            SessionErrorCode::Storage, "Failed to check session: " + exists.error.message));
    }
    if (!exists.value)
    {
        return Result<Session, SessionError>::err_value(
            session_error(SessionErrorCode::NotFound, "Session not found: " + metadata.path));
    }
    auto storage = JsonlSessionStorage::open(fs_, metadata.path);
    if (!storage.ok) return Result<Session, SessionError>::err_value(storage.error);
    return Result<Session, SessionError>::ok_value(
        Session(std::make_shared<JsonlSessionStorage>(std::move(storage.value))));
}

Result<std::vector<SessionMetadata>, SessionError> JsonlSessionRepo::list(
    const std::optional<std::string>& cwd) const
{
    std::vector<std::string> dirs;
    if (cwd)
    {
        const auto dir = get_session_dir(*cwd);
        if (!dir.ok)
            return Result<std::vector<SessionMetadata>, SessionError>::err_value(dir.error);
        dirs.push_back(dir.value);
    }
    else
    {
        if (!root_resolved_)
        {
            const auto resolved = fs_.absolute_path(sessions_root_);
            if (!resolved.ok)
            {
                return Result<std::vector<SessionMetadata>, SessionError>::err_value(
                    session_error(SessionErrorCode::Storage,
                                  "Failed to resolve sessions root: " + resolved.error.message));
            }
            sessions_root_abs_ = resolved.value;
            root_resolved_ = true;
        }
        const auto root_exists = fs_.exists(sessions_root_abs_);
        if (!root_exists.ok)
        {
            return Result<std::vector<SessionMetadata>, SessionError>::err_value(
                session_error(SessionErrorCode::Storage,
                              "Failed to check sessions root: " + root_exists.error.message));
        }
        if (!root_exists.value)
            return Result<std::vector<SessionMetadata>, SessionError>::ok_value({});
        const auto entries = fs_.list_dir(sessions_root_abs_);
        if (!entries.ok)
        {
            return Result<std::vector<SessionMetadata>, SessionError>::err_value(
                session_error(SessionErrorCode::Storage,
                              "Failed to list sessions root: " + entries.error.message));
        }
        for (const auto& entry : entries.value)
        {
            if (entry.kind == FileKind::Directory) dirs.push_back(entry.path);
        }
    }

    std::vector<SessionMetadata> sessions;
    for (const auto& dir : dirs)
    {
        const auto exists = fs_.exists(dir);
        if (!exists.ok || !exists.value) continue;
        const auto files = fs_.list_dir(dir);
        if (!files.ok) continue;
        for (const auto& file : files.value)
        {
            if (file.kind == FileKind::Directory || file.name.size() < 6) continue;
            if (file.name.compare(file.name.size() - 6, 6, ".jsonl") != 0) continue;

            // 流式扫描：只解析 header + 首个用户消息，避免把整份会话（可能含 base64
            // 图片）读入内存再构建完整 entry 列表。
            SessionMetadata metadata;
            metadata.path = file.path;
            bool header_ok = false;
            const auto scanned = fs_.for_each_line(
                file.path,
                [&](const std::string& raw)
                {
                    if (raw.empty()) return true;
                    if (!header_ok)
                    {
                        try
                        {
                            const Json header = Json::parse(raw);
                            if (!header.is_object() || header.value("type", "") != "session" ||
                                header.value("version", 0) != 3)
                                return false;
                            metadata.id = header.value("id", "");
                            metadata.createdAt = header.value("timestamp", "");
                            metadata.cwd = header.value("cwd", "");
                        }
                        catch (...)
                        {
                            return false;  // 坏 header：跳过该会话
                        }
                        header_ok = true;
                        return true;
                    }

                    // 只读顶层 type 字段，不解析整行（图片行可能有 MB 级 base64）
                    if (entry_type_of(raw) != "message") return true;
                    ++metadata.messageCount;
                    if (!metadata.preview.empty()) return true;
                    // 只尝试解析用户消息；图片消息可能含大段 base64，直接跳过
                    if (raw.find("\"role\":\"user\"") == std::string::npos) return true;
                    if (raw.find("\"type\":\"image\"") != std::string::npos) return true;
                    try
                    {
                        const Json entry = Json::parse(raw);
                        if (!entry.contains("message") || !entry["message"].is_object())
                            return true;
                        const Message message = message_from_session_json(entry["message"]);
                        if (message.role != Role::User) return true;
                        const std::string text = message.text_content();
                        if (!text.empty()) metadata.preview = one_line_preview(text);
                    }
                    catch (...)
                    {
                        // 单条坏消息不影响 /sessions 列表
                    }
                    return true;
                });
            if (!scanned.ok || !header_ok) continue;  // 跳过损坏的会话
            sessions.push_back(std::move(metadata));
        }
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionMetadata& a, const SessionMetadata& b)
              {
                  if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
                  return a.id > b.id;  // 同毫秒时按 id 稳定排序
              });
    return Result<std::vector<SessionMetadata>, SessionError>::ok_value(std::move(sessions));
}

Result<void, SessionError> JsonlSessionRepo::remove_session(const SessionMetadata& metadata)
{
    const auto removed = fs_.remove(metadata.path, false);
    if (!removed.ok)
    {
        return Result<void, SessionError>::err_value(session_error(
            SessionErrorCode::Storage, "Failed to delete session: " + removed.error.message));
    }
    return Result<void, SessionError>::ok_value();
}

}  // namespace pi

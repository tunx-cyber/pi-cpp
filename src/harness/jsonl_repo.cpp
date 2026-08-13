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

Result<Session, SessionError> JsonlSessionRepo::create(
    const std::string& cwd, const std::optional<std::string>& parentSessionPath)
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
    auto storage =
        JsonlSessionStorage::create(fs_, file_path.value, cwd, id, parentSessionPath.value_or(""));
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
            const auto storage = JsonlSessionStorage::open(fs_, file.path);
            if (!storage.ok) continue;  // 跳过损坏的会话
            sessions.push_back(storage.value.metadata());
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

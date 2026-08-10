#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pi/harness/session.h"

namespace pi
{

/**
 * 会话仓库：~/.pi-cpp/agent/sessions/<编码后 cwd>/<ts>_<id>.jsonl。
 * 镜像 pi 的 JsonlSessionRepo。
 */
class JsonlSessionRepo
{
   public:
    JsonlSessionRepo(FileSystem& fs, std::string sessionsRoot);

    Result<Session, SessionError> create(
        const std::string& cwd, const std::optional<std::string>& parentSessionPath = std::nullopt);
    Result<Session, SessionError> open(const SessionMetadata& metadata);
    Result<std::vector<SessionMetadata>, SessionError> list(
        const std::optional<std::string>& cwd = std::nullopt) const;
    Result<void, SessionError> remove_session(const SessionMetadata& metadata);

   private:
    Result<std::string, SessionError> get_session_dir(const std::string& cwd) const;
    Result<std::string, SessionError> create_session_file_path(const std::string& cwd,
                                                               const std::string& sessionId,
                                                               const std::string& timestamp) const;

    FileSystem& fs_;
    std::string sessions_root_;
    mutable std::string sessions_root_abs_;
    mutable bool root_resolved_ = false;
};

}  // namespace pi

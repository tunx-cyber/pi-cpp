#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pi/agent/types.h"
#include "pi/ai/types.h"

namespace pi
{

// ---------- Result 工具 ----------

template <typename TValue, typename TError>
struct Result
{
    bool ok = false;
    TValue value{};
    TError error{};

    static Result ok_value(TValue value)
    {
        Result result;
        result.ok = true;
        result.value = std::move(value);
        return result;
    }
    static Result err_value(TError error)
    {
        Result result;
        result.ok = false;
        result.error = std::move(error);
        return result;
    }
};

template <typename TError>
struct Result<void, TError>
{
    bool ok = false;
    TError error{};

    static Result ok_value()
    {
        Result result;
        result.ok = true;
        return result;
    }
    static Result err_value(TError error)
    {
        Result result;
        result.ok = false;
        result.error = std::move(error);
        return result;
    }
};

// ---------- 错误类型 ----------

enum class FileErrorCode
{
    Aborted,
    NotFound,
    PermissionDenied,
    NotDirectory,
    IsDirectory,
    Invalid,
    NotSupported,
    Unknown
};

struct FileError
{
    FileErrorCode code = FileErrorCode::Unknown;
    std::string message;
    std::string path;
};

enum class ExecutionErrorCode
{
    Aborted,
    Timeout,
    ShellUnavailable,
    SpawnError,
    CallbackError,
    Unknown
};

struct ExecutionError
{
    ExecutionErrorCode code = ExecutionErrorCode::Unknown;
    std::string message;
};

enum class CompactionErrorCode
{
    Aborted,
    SummarizationFailed,
    InvalidSession,
    Unknown
};

struct CompactionError
{
    CompactionErrorCode code = CompactionErrorCode::Unknown;
    std::string message;
};

enum class SessionErrorCode
{
    NotFound,
    InvalidSession,
    InvalidEntry,
    InvalidForkTarget,
    Storage,
    Unknown
};

struct SessionError
{
    SessionErrorCode code = SessionErrorCode::Unknown;
    std::string message;
};

// ---------- FileSystem / Shell ----------

enum class FileKind
{
    File,
    Directory,
    Symlink
};

struct FileInfo
{
    std::string name;
    std::string path;
    FileKind kind = FileKind::File;
    int64_t size = 0;
    int64_t mtimeMs = 0;
};

struct ExecOptions
{
    std::string cwd;
    std::map<std::string, std::string> env;
    std::optional<int> timeoutSeconds;
    std::shared_ptr<std::atomic<bool>> abort;
    std::function<void(const std::string&)> onStdout;
    std::function<void(const std::string&)> onStderr;
};

struct ExecResult
{
    std::string stdout;
    std::string stderr;
    int exitCode = 0;
};

/** 文件系统能力接口（POSIX 实现；操作不抛异常，失败编码为 Result）。 */
class FileSystem
{
   public:
    virtual ~FileSystem() = default;
    virtual std::string cwd() const = 0;
    virtual Result<std::string, FileError> absolute_path(const std::string& path) = 0;
    virtual Result<std::string, FileError> join_path(const std::vector<std::string>& parts) = 0;
    virtual Result<std::string, FileError> read_text_file(const std::string& path) = 0;
    virtual Result<std::vector<std::string>, FileError> read_text_lines(const std::string& path,
                                                                        int maxLines = -1) = 0;
    virtual Result<void, FileError> write_file(const std::string& path,
                                               const std::string& content) = 0;
    virtual Result<void, FileError> append_file(const std::string& path,
                                                const std::string& content) = 0;
    virtual Result<FileInfo, FileError> file_info(const std::string& path) = 0;
    virtual Result<std::vector<FileInfo>, FileError> list_dir(const std::string& path) = 0;
    virtual Result<std::string, FileError> canonical_path(const std::string& path) = 0;
    virtual Result<bool, FileError> exists(const std::string& path) = 0;
    virtual Result<void, FileError> create_dir(const std::string& path, bool recursive = true) = 0;
    virtual Result<void, FileError> remove(const std::string& path, bool recursive = false) = 0;
};

/** Shell 执行能力。 */
class Shell
{
   public:
    virtual ~Shell() = default;
    virtual Result<ExecResult, ExecutionError> exec(const std::string& command,
                                                    const ExecOptions& options) = 0;
};

class ExecutionEnv : public FileSystem, public Shell
{
};

// ---------- 会话树条目 ----------

struct SessionTreeEntry
{
    enum class Type
    {
        Message,
        ThinkingLevelChange,
        ModelChange,
        ActiveToolsChange,
        Compaction,
        BranchSummary,
        Custom,
        CustomMessage,
        Label,
        SessionInfo,
        Leaf,
    };

    Type type = Type::Message;
    std::string id;
    std::string parentId;   // 空串 = null
    std::string timestamp;  // ISO 8601

    // message
    AgentMessage message;
    // thinking_level_change
    std::string thinkingLevel;
    // model_change
    std::string provider;
    std::string modelId;
    // active_tools_change
    std::vector<std::string> activeToolNames;
    // compaction
    std::string summary;
    std::string firstKeptEntryId;
    int64_t tokensBefore = 0;
    Json details = Json::object();
    bool fromHook = false;
    // branch_summary
    std::string fromId;
    // custom / custom_message
    std::string customType;
    bool display = false;
    // label
    std::string targetId;
    std::string label;  // 空串 = undefined
    // session_info
    std::string name;
    // leaf
    std::string leafTargetId;  // 空串 = null
};

struct SessionContext
{
    std::vector<AgentMessage> messages;
    std::string thinkingLevel = "off";
    std::optional<std::pair<std::string, std::string>> model;  // provider/modelId
    std::optional<std::vector<std::string>> activeToolNames;
};

struct SessionMetadata
{
    std::string id;
    std::string createdAt;
    std::string cwd;
    std::string path;
    std::string parentSessionPath;
};

// ---------- Skills / Templates ----------

struct Skill
{
    std::string name;
    std::string description;
    std::string content;
    std::string filePath;
    bool disableModelInvocation = false;
};

struct PromptTemplate
{
    std::string name;
    std::string description;
    std::string content;
};

// ---------- Compaction ----------

struct CompactionSettings
{
    bool enabled = true;
    int64_t reserveTokens = 16384;
    int64_t keepRecentTokens = 20000;
};

struct FileOperations
{
    std::set<std::string> read;
    std::set<std::string> written;
    std::set<std::string> edited;
};

struct CompactionPreparation
{
    std::string firstKeptEntryId;
    std::vector<AgentMessage> messagesToSummarize;
    std::vector<AgentMessage> turnPrefixMessages;
    bool isSplitTurn = false;
    int64_t tokensBefore = 0;
    std::string previousSummary;
    FileOperations fileOps;
    CompactionSettings settings;
};

struct CompactionResult
{
    std::string summary;
    std::string firstKeptEntryId;
    int64_t tokensBefore = 0;
    Json details = Json::object();
};

}  // namespace pi

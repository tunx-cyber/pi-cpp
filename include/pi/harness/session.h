#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pi/harness/types.h"

namespace pi
{

/** 会话条目 ↔ wire JSON（镜像 pi 的 JSONL 格式）。解析失败抛 SessionError。 */
Json session_entry_to_json(const SessionTreeEntry& entry);
SessionTreeEntry session_entry_from_json(const Json& json, const std::string& filePath,
                                         int lineNumber);

/** ISO 8601 UTC 时间戳（镜像 new Date().toISOString()）。 */
std::string iso_timestamp();

/**
 * JSONL 会话存储：append-only，version-3 头部，leaf 跟踪。
 * 镜像 pi 的 JsonlSessionStorage。
 *
 * 线程安全：REPL 中 run 线程（消息持久化）与 UI 线程（/model、/thinking、
 * /tools 等切换命令）会并发访问同一存储，所有公开方法均由 mutex_ 保护。
 * append_child 在单次加锁内完成「生成 id → 挂到当前 leaf → 落盘」，
 * 保证并发追加时历史链保持线性、不会产生孤儿分支。
 */
class JsonlSessionStorage
{
   public:
    static Result<JsonlSessionStorage, SessionError> open(FileSystem& fs, const std::string& path);
    static Result<JsonlSessionStorage, SessionError> create(FileSystem& fs, const std::string& path,
                                                            const std::string& cwd,
                                                            const std::string& sessionId,
                                                            const std::string& parentSessionPath);

    SessionMetadata metadata() const { return metadata_; }

    // 移动语义：open()/create() 把构造好的存储交给 Result/Session。
    // 拷贝被禁用（mutex_ 不可复制）；会话对象之间经 Session 的 shared_ptr 共享同一存储。
    JsonlSessionStorage() = default;
    JsonlSessionStorage(JsonlSessionStorage&& other) noexcept;
    JsonlSessionStorage& operator=(JsonlSessionStorage&& other) noexcept;
    JsonlSessionStorage(const JsonlSessionStorage&) = delete;
    JsonlSessionStorage& operator=(const JsonlSessionStorage&) = delete;

    Result<std::string, SessionError> get_leaf_id() const;  // 空串 = null
    Result<void, SessionError> set_leaf_id(const std::string& leafId);
    std::string create_entry_id();
    Result<void, SessionError> append_entry(SessionTreeEntry entry);
    /** 以当前 leaf 为父节点原子追加条目（自动分配 id/parentId/timestamp），返回新条目 id。 */
    Result<std::string, SessionError> append_child(SessionTreeEntry entry);
    Result<std::optional<SessionTreeEntry>, SessionError> get_entry(const std::string& id) const;
    Result<std::vector<SessionTreeEntry>, SessionError> find_entries(
        SessionTreeEntry::Type type) const;
    Result<std::optional<std::string>, SessionError> get_label(const std::string& id) const;
    Result<std::vector<SessionTreeEntry>, SessionError> get_path_to_root(
        const std::string& leafId) const;
    Result<std::vector<SessionTreeEntry>, SessionError> get_entries() const;

   private:
    /** 无锁内部实现；调用方必须已持有 mutex_。 */
    Result<void, SessionError> append_entry_locked(SessionTreeEntry entry);

    FileSystem* fs_;
    std::string file_path_;
    SessionMetadata metadata_;
    mutable std::mutex mutex_;
    std::vector<SessionTreeEntry> entries_;
    std::map<std::string, size_t> by_id_;
    std::map<std::string, std::string> labels_by_id_;
    std::string current_leaf_id_;
};

/** 会话：append 各类条目 + build_context 重放，镜像 pi 的 Session。 */
class Session
{
   public:
    Session() = default;
    explicit Session(std::shared_ptr<JsonlSessionStorage> storage) : storage_(std::move(storage)) {}

    SessionMetadata metadata() const;
    Result<std::string, SessionError> get_leaf_id() const;
    Result<std::vector<SessionTreeEntry>, SessionError> get_entries() const;
    Result<std::optional<SessionTreeEntry>, SessionError> get_entry(const std::string& id) const;
    Result<std::vector<SessionTreeEntry>, SessionError> get_branch(
        const std::string& fromId = "") const;
    Result<SessionContext, SessionError> build_context() const;
    Result<std::optional<std::string>, SessionError> get_label(const std::string& id) const;
    Result<std::optional<std::string>, SessionError> get_session_name() const;

    Result<std::string, SessionError> append_message(const AgentMessage& message);
    Result<std::string, SessionError> append_thinking_level_change(const std::string& level);
    Result<std::string, SessionError> append_model_change(const std::string& provider,
                                                          const std::string& modelId);
    Result<std::string, SessionError> append_active_tools_change(
        const std::vector<std::string>& names);
    Result<std::string, SessionError> append_compaction(const std::string& summary,
                                                        const std::string& firstKeptEntryId,
                                                        int64_t tokensBefore, const Json& details,
                                                        bool fromHook);
    Result<std::string, SessionError> append_custom_entry(const std::string& customType,
                                                          const Json& data);
    Result<std::string, SessionError> append_custom_message_entry(const std::string& customType,
                                                                  const std::string& content,
                                                                  bool display);
    Result<std::string, SessionError> append_label(const std::string& targetId,
                                                   const std::string& label);
    Result<std::string, SessionError> append_session_name(const std::string& name);
    Result<std::optional<std::string>, SessionError> move_to(
        const std::string& entryId, const std::optional<std::string>& summary);

   private:
    Result<std::string, SessionError> append_typed_entry(SessionTreeEntry entry);
    std::shared_ptr<JsonlSessionStorage> storage_;
};

/** 消息 → wire JSON（pi 形状，存于 session 条目）。 */
Json message_to_session_json(const Message& message);
Message message_from_session_json(const Json& json);

}  // namespace pi

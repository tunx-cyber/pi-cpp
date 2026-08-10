#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pi/ai/transport_adapter.h"
#include "pi/harness/types.h"

namespace pi
{

// 消息摘要前缀/后缀（镜像 pi 的 messages.ts）
inline constexpr const char* kCompactionSummaryPrefix =
    "The conversation history before this point was compacted into the following "
    "summary:\n\n<summary>\n";
inline constexpr const char* kCompactionSummarySuffix = "\n</summary>";

/** 默认压缩设置。 */
inline CompactionSettings default_compaction_settings()
{
    CompactionSettings settings;
    settings.enabled = true;
    settings.reserveTokens = 16384;
    settings.keepRecentTokens = 20000;
    return settings;
}

/** 从 provider usage 计算上下文 token（镜像 calculateContextTokens）。 */
int64_t calculate_context_tokens(const Usage& usage);

/** 单条消息 token 估算（chars/4，图片 4800；镜像 estimateTokens）。 */
int64_t estimate_tokens(const AgentMessage& message);

struct ContextUsageEstimate
{
    int64_t tokens = 0;
    int64_t usageTokens = 0;
    int64_t trailingTokens = 0;
    int lastUsageIndex = -1;
};

/** 估算消息列表上下文 token（使用最近的 assistant usage）。 */
ContextUsageEstimate estimate_context_tokens(const std::vector<AgentMessage>& messages);

/** 是否应压缩：enabled && tokens > contextWindow - reserveTokens。 */
bool should_compact(int64_t contextTokens, int64_t contextWindow,
                    const CompactionSettings& settings);

struct CutPointResult
{
    int firstKeptEntryIndex = 0;
    int turnStartIndex = -1;
    bool isSplitTurn = false;
};

/** 找压缩切点（镜像 findCutPoint）。 */
CutPointResult find_cut_point(const std::vector<SessionTreeEntry>& entries, int startIndex,
                              int endIndex, int64_t keepRecentTokens);

/** 会话条目 → 消息（镜像 getMessageFromEntry）。 */
std::optional<AgentMessage> get_message_from_entry(const SessionTreeEntry& entry,
                                                   bool forCompaction);

/** 准备压缩（镜像 prepareCompaction）。 */
Result<std::optional<CompactionPreparation>, CompactionError> prepare_compaction(
    const std::vector<SessionTreeEntry>& pathEntries, const CompactionSettings& settings);

/** 生成压缩摘要（LLM 调用；镜像 generateSummary）。 */
Result<std::string, CompactionError> generate_summary(
    const std::vector<AgentMessage>& currentMessages, const ModelInfo& model, int64_t reserveTokens,
    const std::shared_ptr<TransportAdapter>& transport, const std::optional<std::string>& apiKey,
    const std::shared_ptr<std::atomic<bool>>& signal, const std::string& customInstructions = "",
    const std::string& previousSummary = "",
    std::optional<ThinkingLevel> thinkingLevel = std::nullopt);

/** 执行压缩（镜像 compact）。 */
Result<CompactionResult, CompactionError> compact(
    const CompactionPreparation& preparation, const ModelInfo& model,
    const std::shared_ptr<TransportAdapter>& transport, const std::optional<std::string>& apiKey,
    const std::shared_ptr<std::atomic<bool>>& signal, const std::string& customInstructions = "",
    std::optional<ThinkingLevel> thinkingLevel = std::nullopt);

/** 会话 → 纯文本（镜像 serializeConversation）。 */
std::string serialize_conversation(const std::vector<Message>& messages);

/** 文件操作提取/格式化（镜像 compaction/utils.ts）。 */
void extract_file_ops_from_message(const AgentMessage& message, FileOperations& fileOps);
std::pair<std::vector<std::string>, std::vector<std::string>> compute_file_lists(
    const FileOperations& fileOps);
std::string format_file_operations(const std::vector<std::string>& readFiles,
                                   const std::vector<std::string>& modifiedFiles);

}  // namespace pi

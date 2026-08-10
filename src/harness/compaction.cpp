#include "pi/harness/compaction.h"

#include <cmath>

#include <algorithm>

#include "pi/ai/json_util.h"
#include "pi/harness/session.h"

namespace pi
{

namespace
{

CompactionError compaction_error(CompactionErrorCode code, const std::string& message)
{
    CompactionError error;
    error.code = code;
    error.message = message;
    return error;
}

constexpr int64_t kEstimatedImageChars = 4800;
constexpr int64_t kToolResultMaxChars = 2000;

constexpr const char* kSummarizationSystemPrompt =
    "You are a context summarization assistant. Your task is to read a conversation between a user "
    "and an AI coding "
    "assistant, then produce a structured summary following the exact format specified.\n\nDo NOT "
    "continue the "
    "conversation. Do NOT respond to any questions in the conversation. ONLY output the structured "
    "summary.";

constexpr const char* kSummarizationPrompt =
    "The messages above are a conversation to summarize. Create a structured context checkpoint "
    "summary that another "
    "LLM will use to continue the work.\n\nUse this EXACT format:\n\n## Goal\n[What is the user "
    "trying to accomplish? "
    "Can be multiple items if the session covers different tasks.]\n\n## Constraints & "
    "Preferences\n- [Any "
    "constraints, preferences, or requirements mentioned by user]\n- [Or \"(none)\" if none were "
    "mentioned]\n\n## "
    "Progress\n### Done\n- [x] [Completed tasks/changes]\n\n### In Progress\n- [ ] [Current "
    "work]\n\n### Blocked\n- "
    "[Issues preventing progress, if any]\n\n## Key Decisions\n- **[Decision]**: [Brief "
    "rationale]\n\n## Next "
    "Steps\n1. [Ordered list of what should happen next]\n\n## Critical Context\n- [Any data, "
    "examples, or references "
    "needed to continue]\n- [Or \"(none)\" if not applicable]\n\nKeep each section concise. "
    "Preserve exact file paths, "
    "function names, and error messages.";

constexpr const char* kUpdateSummarizationPrompt =
    "The messages above are NEW conversation messages to incorporate into the existing summary "
    "provided in "
    "<previous-summary> tags.\n\nUpdate the existing structured summary with new information. "
    "RULES:\n- PRESERVE all "
    "existing information from the previous summary\n- ADD new progress, decisions, and context "
    "from the new "
    "messages\n- UPDATE the Progress section: move items from \"In Progress\" to \"Done\" when "
    "completed\n- UPDATE "
    "\"Next Steps\" based on what was accomplished\n- PRESERVE exact file paths, function names, "
    "and error "
    "messages\n- If something is no longer relevant, you may remove it\n\nUse this EXACT "
    "format:\n\n## Goal\n[Preserve "
    "existing goals, add new ones if the task expanded]\n\n## Constraints & Preferences\n- "
    "[Preserve existing, add new "
    "ones discovered]\n\n## Progress\n### Done\n- [x] [Include previously done items AND newly "
    "completed items]\n\n### "
    "In Progress\n- [ ] [Current work - update based on progress]\n\n### Blocked\n- [Current "
    "blockers - remove if "
    "resolved]\n\n## Key Decisions\n- **[Decision]**: [Brief rationale] (preserve all previous, "
    "add new)\n\n## Next "
    "Steps\n1. [Update based on current state]\n\n## Critical Context\n- [Preserve important "
    "context, add new if "
    "needed]\n\nKeep each section concise. Preserve exact file paths, function names, and error "
    "messages.";

constexpr const char* kTurnPrefixSummarizationPrompt =
    "This is the PREFIX of a turn that was too large to keep. The SUFFIX (recent work) is "
    "retained.\n\nSummarize the "
    "prefix to provide context for the retained suffix:\n\n## Original Request\n[What did the user "
    "ask for in this "
    "turn?]\n\n## Early Progress\n- [Key decisions and work done in the prefix]\n\n## Context for "
    "Suffix\n- "
    "[Information needed to understand the retained recent work]\n\nBe concise. Focus on what's "
    "needed to understand "
    "the kept suffix.";

int64_t estimate_text_and_image_chars(const std::vector<ContentBlock>& content)
{
    int64_t chars = 0;
    for (const auto& block : content)
    {
        if (block.type == BlockType::Text)
        {
            chars += static_cast<int64_t>(block.text.size());
        }
        else if (block.type == BlockType::Image)
        {
            chars += kEstimatedImageChars;
        }
    }
    return chars;
}

bool is_whitespace_only(const std::string& s)
{
    for (char c : s)
    {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

std::string safe_json_stringify(const Json& value)
{
    try
    {
        return value.dump();
    }
    catch (...)
    {
        return "[unserializable]";
    }
}

std::string truncate_for_summary(const std::string& text, int64_t maxChars)
{
    if (static_cast<int64_t>(text.size()) <= maxChars) return text;
    const int64_t truncated = static_cast<int64_t>(text.size()) - maxChars;
    return text.substr(0, static_cast<size_t>(maxChars)) + "\n\n[... " + std::to_string(truncated) +
           " more characters truncated]";
}

bool is_cut_point_entry(const SessionTreeEntry& entry)
{
    switch (entry.type)
    {
        case SessionTreeEntry::Type::Message:
        {
            const auto role = entry.message.role;
            return role == Role::User || role == Role::Assistant;
        }
        case SessionTreeEntry::Type::BranchSummary:
        case SessionTreeEntry::Type::CustomMessage:
            return true;
        default:
            return false;
    }
}

int find_turn_start_index(const std::vector<SessionTreeEntry>& entries, int entryIndex,
                          int startIndex)
{
    for (int i = entryIndex; i >= startIndex; --i)
    {
        const auto& entry = entries[i];
        if (entry.type == SessionTreeEntry::Type::BranchSummary ||
            entry.type == SessionTreeEntry::Type::CustomMessage)
        {
            return i;
        }
        if (entry.type == SessionTreeEntry::Type::Message && entry.message.role == Role::User)
        {
            return i;
        }
    }
    return -1;
}

std::optional<Usage> get_assistant_usage(const AgentMessage& message)
{
    if (message.role == Role::Assistant &&
        (message.stopReason == StopReason::Stop || message.stopReason == StopReason::Length ||
         message.stopReason == StopReason::ToolUse))
    {
        return message.usage;
    }
    return std::nullopt;
}

Result<std::string, CompactionError> generate_summary_internal(
    const std::vector<AgentMessage>& messages, const ModelInfo& model, int64_t maxTokens,
    const std::shared_ptr<TransportAdapter>& transport, const std::optional<std::string>& apiKey,
    const std::shared_ptr<std::atomic<bool>>& signal, const std::string& promptBase,
    const std::string& conversationText, std::optional<ThinkingLevel> thinkingLevel)
{
    std::string prompt_text =
        "<conversation>\n" + conversationText + "\n</conversation>\n\n" + promptBase;

    StreamRequestOptions opts;
    opts.apiKey = apiKey;
    opts.maxTokens = static_cast<int>(
        std::min<int64_t>(maxTokens, model.maxTokens > 0 ? model.maxTokens : maxTokens));
    opts.systemPrompt = kSummarizationSystemPrompt;
    opts.abort = signal;
    opts.reasoning =
        thinkingLevel && *thinkingLevel != ThinkingLevel::Off ? thinkingLevel : std::nullopt;

    const Message result =
        transport->complete_chat(model, {Message::user(std::move(prompt_text))}, opts);
    if (result.stopReason == StopReason::Aborted)
    {
        return Result<std::string, CompactionError>::err_value(compaction_error(
            CompactionErrorCode::Aborted,
            result.errorMessage.empty() ? "Summarization aborted" : result.errorMessage));
    }
    if (result.stopReason == StopReason::Error)
    {
        return Result<std::string, CompactionError>::err_value(compaction_error(
            CompactionErrorCode::SummarizationFailed,
            "Summarization failed: " +
                (result.errorMessage.empty() ? "Unknown error" : result.errorMessage)));
    }
    std::string text;
    for (const auto& block : result.content)
    {
        if (block.type == BlockType::Text) text += block.text;
    }
    return Result<std::string, CompactionError>::ok_value(std::move(text));
}

}  // namespace

int64_t calculate_context_tokens(const Usage& usage)
{
    const int64_t sum = usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
    return usage.totalTokens > 0 ? usage.totalTokens : sum;
}

int64_t estimate_tokens(const AgentMessage& message)
{
    int64_t chars = 0;
    switch (message.role)
    {
        case Role::User:
            return static_cast<int64_t>(
                std::ceil(estimate_text_and_image_chars(message.content) / 4.0));
        case Role::Assistant:
        {
            for (const auto& block : message.content)
            {
                if (block.type == BlockType::Text)
                {
                    chars += static_cast<int64_t>(block.text.size());
                }
                else if (block.type == BlockType::Thinking)
                {
                    chars += static_cast<int64_t>(block.thinking.size());
                }
                else if (block.type == BlockType::ToolCall)
                {
                    chars += static_cast<int64_t>(block.name.size()) +
                             static_cast<int64_t>(safe_json_stringify(block.arguments).size());
                }
            }
            return static_cast<int64_t>(std::ceil(chars / 4.0));
        }
        case Role::ToolResult:
            return static_cast<int64_t>(
                std::ceil(estimate_text_and_image_chars(message.content) / 4.0));
    }
    return 0;
}

ContextUsageEstimate estimate_context_tokens(const std::vector<AgentMessage>& messages)
{
    int usage_index = -1;
    Usage usage;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i)
    {
        const auto maybe_usage = get_assistant_usage(messages[i]);
        if (maybe_usage.has_value())
        {
            usage = *maybe_usage;
            usage_index = i;
            break;
        }
    }

    if (usage_index < 0)
    {
        int64_t estimated = 0;
        for (const auto& message : messages) estimated += estimate_tokens(message);
        ContextUsageEstimate result;
        result.tokens = estimated;
        result.usageTokens = 0;
        result.trailingTokens = estimated;
        result.lastUsageIndex = -1;
        return result;
    }

    const int64_t usage_tokens = calculate_context_tokens(usage);
    int64_t trailing = 0;
    for (int i = usage_index + 1; i < static_cast<int>(messages.size()); ++i)
    {
        trailing += estimate_tokens(messages[i]);
    }
    ContextUsageEstimate result;
    result.tokens = usage_tokens + trailing;
    result.usageTokens = usage_tokens;
    result.trailingTokens = trailing;
    result.lastUsageIndex = usage_index;
    return result;
}

bool should_compact(int64_t contextTokens, int64_t contextWindow,
                    const CompactionSettings& settings)
{
    if (!settings.enabled) return false;
    return contextTokens > contextWindow - settings.reserveTokens;
}

CutPointResult find_cut_point(const std::vector<SessionTreeEntry>& entries, int startIndex,
                              int endIndex, int64_t keepRecentTokens)
{
    std::vector<int> cut_points;
    for (int i = startIndex; i < endIndex; ++i)
    {
        if (is_cut_point_entry(entries[i])) cut_points.push_back(i);
    }

    if (cut_points.empty())
    {
        return {startIndex, -1, false};
    }
    int64_t accumulated = 0;
    int cut_index = cut_points[0];

    for (int i = endIndex - 1; i >= startIndex; --i)
    {
        if (entries[i].type != SessionTreeEntry::Type::Message) continue;
        accumulated += estimate_tokens(entries[i].message);
        if (accumulated >= keepRecentTokens)
        {
            for (int c = 0; c < static_cast<int>(cut_points.size()); ++c)
            {
                if (cut_points[c] >= i)
                {
                    cut_index = cut_points[c];
                    break;
                }
            }
            break;
        }
    }
    while (cut_index > startIndex)
    {
        const auto& prev = entries[cut_index - 1];
        if (prev.type == SessionTreeEntry::Type::Compaction) break;
        if (prev.type == SessionTreeEntry::Type::Message) break;
        --cut_index;
    }

    const auto& cut_entry = entries[cut_index];
    const bool is_user_message =
        cut_entry.type == SessionTreeEntry::Type::Message && cut_entry.message.role == Role::User;
    const int turn_start_index =
        is_user_message ? -1 : find_turn_start_index(entries, cut_index, startIndex);

    CutPointResult result;
    result.firstKeptEntryIndex = cut_index;
    result.turnStartIndex = turn_start_index;
    result.isSplitTurn = !is_user_message && turn_start_index != -1;
    return result;
}

std::optional<AgentMessage> get_message_from_entry(const SessionTreeEntry& entry,
                                                   bool forCompaction)
{
    switch (entry.type)
    {
        case SessionTreeEntry::Type::Message:
            return entry.message;
        case SessionTreeEntry::Type::CustomMessage:
        {
            Message message;
            message.role = Role::User;
            message.content.push_back(ContentBlock{});
            message.content.back().type = BlockType::Text;
            message.content.back().text = entry.message.text_content();
            return message;
        }
        case SessionTreeEntry::Type::BranchSummary:
        {
            Message message;
            message.role = Role::User;
            message.content.push_back(ContentBlock{});
            message.content.back().type = BlockType::Text;
            message.content.back().text =
                "The following is a summary of a branch that this conversation came back "
                "from:\n\n<summary>\n" +
                entry.summary + "</summary>";
            return message;
        }
        case SessionTreeEntry::Type::Compaction:
            if (forCompaction) return std::nullopt;
            {
                Message message;
                message.role = Role::User;
                message.content.push_back(ContentBlock{});
                message.content.back().type = BlockType::Text;
                message.content.back().text = std::string(kCompactionSummaryPrefix) +
                                              entry.summary + kCompactionSummarySuffix;
                return message;
            }
        default:
            return std::nullopt;
    }
}

Result<std::optional<CompactionPreparation>, CompactionError> prepare_compaction(
    const std::vector<SessionTreeEntry>& pathEntries, const CompactionSettings& settings)
{
    if (pathEntries.empty() || pathEntries.back().type == SessionTreeEntry::Type::Compaction)
    {
        return Result<std::optional<CompactionPreparation>, CompactionError>::ok_value(
            std::nullopt);
    }

    int prev_compaction_index = -1;
    for (int i = static_cast<int>(pathEntries.size()) - 1; i >= 0; --i)
    {
        if (pathEntries[i].type == SessionTreeEntry::Type::Compaction)
        {
            prev_compaction_index = i;
            break;
        }
    }

    std::string previous_summary;
    int boundary_start = 0;
    if (prev_compaction_index >= 0)
    {
        previous_summary = pathEntries[prev_compaction_index].summary;
        int first_kept_index = -1;
        for (int i = 0; i < static_cast<int>(pathEntries.size()); ++i)
        {
            if (pathEntries[i].id == pathEntries[prev_compaction_index].firstKeptEntryId)
            {
                first_kept_index = i;
                break;
            }
        }
        boundary_start = first_kept_index >= 0 ? first_kept_index : prev_compaction_index + 1;
    }
    const int boundary_end = static_cast<int>(pathEntries.size());

    // tokensBefore：从 build_context 等价重放的消息估算
    int64_t tokens_before = 0;
    {
        std::vector<AgentMessage> context_messages;
        const SessionTreeEntry* compaction_entry = nullptr;
        for (const auto& entry : pathEntries)
        {
            if (entry.type == SessionTreeEntry::Type::Compaction) compaction_entry = &entry;
        }
        bool found_first_kept = compaction_entry == nullptr;
        for (const auto& entry : pathEntries)
        {
            if (compaction_entry && entry.id == compaction_entry->firstKeptEntryId)
                found_first_kept = true;
            if (!found_first_kept) continue;
            const auto message = get_message_from_entry(entry, false);
            if (message) context_messages.push_back(*message);
        }
        tokens_before = estimate_context_tokens(context_messages).tokens;
    }

    const auto cut_point =
        find_cut_point(pathEntries, boundary_start, boundary_end, settings.keepRecentTokens);
    const auto& first_kept_entry = pathEntries[cut_point.firstKeptEntryIndex];
    if (first_kept_entry.id.empty())
    {
        return Result<std::optional<CompactionPreparation>, CompactionError>::err_value(
            compaction_error(CompactionErrorCode::InvalidSession,
                             "First kept entry has no UUID - session may need migration"));
    }
    const std::string first_kept_entry_id = first_kept_entry.id;

    const int history_end =
        cut_point.isSplitTurn ? cut_point.turnStartIndex : cut_point.firstKeptEntryIndex;
    std::vector<AgentMessage> messages_to_summarize;
    for (int i = boundary_start; i < history_end; ++i)
    {
        const auto message = get_message_from_entry(pathEntries[i], true);
        if (message) messages_to_summarize.push_back(*message);
    }
    std::vector<AgentMessage> turn_prefix_messages;
    if (cut_point.isSplitTurn)
    {
        for (int i = cut_point.turnStartIndex; i < cut_point.firstKeptEntryIndex; ++i)
        {
            const auto message = get_message_from_entry(pathEntries[i], true);
            if (message) turn_prefix_messages.push_back(*message);
        }
    }

    FileOperations file_ops;
    if (prev_compaction_index >= 0)
    {
        const auto& prev = pathEntries[prev_compaction_index];
        if (!prev.fromHook && prev.details.is_object())
        {
            if (prev.details.contains("readFiles") && prev.details["readFiles"].is_array())
            {
                for (const auto& f : prev.details["readFiles"])
                    file_ops.read.insert(f.get<std::string>());
            }
            if (prev.details.contains("modifiedFiles") && prev.details["modifiedFiles"].is_array())
            {
                for (const auto& f : prev.details["modifiedFiles"])
                    file_ops.edited.insert(f.get<std::string>());
            }
        }
    }
    for (const auto& message : messages_to_summarize)
        extract_file_ops_from_message(message, file_ops);
    if (cut_point.isSplitTurn)
    {
        for (const auto& message : turn_prefix_messages)
            extract_file_ops_from_message(message, file_ops);
    }

    CompactionPreparation preparation;
    preparation.firstKeptEntryId = first_kept_entry_id;
    preparation.messagesToSummarize = std::move(messages_to_summarize);
    preparation.turnPrefixMessages = std::move(turn_prefix_messages);
    preparation.isSplitTurn = cut_point.isSplitTurn;
    preparation.tokensBefore = tokens_before;
    preparation.previousSummary = previous_summary;
    preparation.fileOps = std::move(file_ops);
    preparation.settings = settings;
    return Result<std::optional<CompactionPreparation>, CompactionError>::ok_value(
        std::move(preparation));
}

std::string serialize_conversation(const std::vector<Message>& messages)
{
    std::vector<std::string> parts;
    for (const auto& msg : messages)
    {
        if (msg.role == Role::User)
        {
            std::string content;
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Text) content += block.text;
            }
            if (!content.empty()) parts.push_back("[User]: " + content);
        }
        else if (msg.role == Role::Assistant)
        {
            std::vector<std::string> text_parts, thinking_parts, tool_calls;
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Text)
                {
                    text_parts.push_back(block.text);
                }
                else if (block.type == BlockType::Thinking)
                {
                    thinking_parts.push_back(block.thinking);
                }
                else if (block.type == BlockType::ToolCall)
                {
                    std::vector<std::string> args;
                    for (auto it = block.arguments.begin(); it != block.arguments.end(); ++it)
                    {
                        args.push_back(it.key() + "=" + safe_json_stringify(it.value()));
                    }
                    std::string args_str;
                    for (size_t i = 0; i < args.size(); ++i)
                    {
                        if (i > 0) args_str += ", ";
                        args_str += args[i];
                    }
                    tool_calls.push_back(block.name + "(" + args_str + ")");
                }
            }
            std::string joined_thinking;
            for (size_t i = 0; i < thinking_parts.size(); ++i)
            {
                if (i > 0) joined_thinking += "\n";
                joined_thinking += thinking_parts[i];
            }
            std::string joined_text;
            for (size_t i = 0; i < text_parts.size(); ++i)
            {
                if (i > 0) joined_text += "\n";
                joined_text += text_parts[i];
            }
            std::string joined_calls;
            for (size_t i = 0; i < tool_calls.size(); ++i)
            {
                if (i > 0) joined_calls += "; ";
                joined_calls += tool_calls[i];
            }
            if (!joined_thinking.empty())
                parts.push_back("[Assistant thinking]: " + joined_thinking);
            if (!joined_text.empty()) parts.push_back("[Assistant]: " + joined_text);
            if (!joined_calls.empty()) parts.push_back("[Assistant tool calls]: " + joined_calls);
        }
        else if (msg.role == Role::ToolResult)
        {
            std::string content;
            for (const auto& block : msg.content)
            {
                if (block.type == BlockType::Text) content += block.text;
            }
            if (!content.empty())
                parts.push_back("[Tool result]: " +
                                truncate_for_summary(content, kToolResultMaxChars));
        }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0) out += "\n\n";
        out += parts[i];
    }
    return out;
}

void extract_file_ops_from_message(const AgentMessage& message, FileOperations& fileOps)
{
    if (message.role != Role::Assistant) return;
    for (const auto& block : message.content)
    {
        if (block.type != BlockType::ToolCall) continue;
        if (!block.arguments.is_object()) continue;
        const auto path_it = block.arguments.find("path");
        if (path_it == block.arguments.end() || !path_it->is_string()) continue;
        const std::string path = path_it->get<std::string>();
        if (block.name == "read")
        {
            fileOps.read.insert(path);
        }
        else if (block.name == "write")
        {
            fileOps.written.insert(path);
        }
        else if (block.name == "edit")
        {
            fileOps.edited.insert(path);
        }
    }
}

std::pair<std::vector<std::string>, std::vector<std::string>> compute_file_lists(
    const FileOperations& fileOps)
{
    std::set<std::string> modified_set = fileOps.edited;
    modified_set.insert(fileOps.written.begin(), fileOps.written.end());
    std::vector<std::string> read_only;
    for (const auto& f : fileOps.read)
    {
        if (modified_set.find(f) == modified_set.end()) read_only.push_back(f);
    }
    std::vector<std::string> modified(modified_set.begin(), modified_set.end());
    std::sort(read_only.begin(), read_only.end());
    std::sort(modified.begin(), modified.end());
    return {std::move(read_only), std::move(modified)};
}

std::string format_file_operations(const std::vector<std::string>& readFiles,
                                   const std::vector<std::string>& modifiedFiles)
{
    std::vector<std::string> sections;
    if (!readFiles.empty())
    {
        std::string section = "<read-files>\n";
        for (size_t i = 0; i < readFiles.size(); ++i)
        {
            if (i > 0) section += "\n";
            section += readFiles[i];
        }
        section += "\n</read-files>";
        sections.push_back(std::move(section));
    }
    if (!modifiedFiles.empty())
    {
        std::string section = "<modified-files>\n";
        for (size_t i = 0; i < modifiedFiles.size(); ++i)
        {
            if (i > 0) section += "\n";
            section += modifiedFiles[i];
        }
        section += "\n</modified-files>";
        sections.push_back(std::move(section));
    }
    if (sections.empty()) return "";
    std::string out = "\n\n";
    for (size_t i = 0; i < sections.size(); ++i)
    {
        if (i > 0) out += "\n\n";
        out += sections[i];
    }
    return out;
}

Result<std::string, CompactionError> generate_summary(
    const std::vector<AgentMessage>& currentMessages, const ModelInfo& model, int64_t reserveTokens,
    const std::shared_ptr<TransportAdapter>& transport, const std::optional<std::string>& apiKey,
    const std::shared_ptr<std::atomic<bool>>& signal, const std::string& customInstructions,
    const std::string& previousSummary, std::optional<ThinkingLevel> thinkingLevel)
{
    const int64_t max_tokens = static_cast<int64_t>(std::floor(0.8 * reserveTokens));
    std::string base_prompt =
        previousSummary.empty() ? kSummarizationPrompt : kUpdateSummarizationPrompt;
    if (!customInstructions.empty())
    {
        base_prompt += "\n\nAdditional focus: " + customInstructions;
    }
    const std::string conversation_text = serialize_conversation(currentMessages);
    std::string prompt_text = "<conversation>\n" + conversation_text + "\n</conversation>\n\n";
    if (!previousSummary.empty())
    {
        prompt_text += "<previous-summary>\n" + previousSummary + "\n</previous-summary>\n\n";
    }
    prompt_text += base_prompt;
    return generate_summary_internal(currentMessages, model, max_tokens, transport, apiKey, signal,
                                     prompt_text, conversation_text, thinkingLevel);
}

Result<CompactionResult, CompactionError> compact(
    const CompactionPreparation& preparation, const ModelInfo& model,
    const std::shared_ptr<TransportAdapter>& transport, const std::optional<std::string>& apiKey,
    const std::shared_ptr<std::atomic<bool>>& signal, const std::string& customInstructions,
    std::optional<ThinkingLevel> thinkingLevel)
{
    if (preparation.firstKeptEntryId.empty())
    {
        return Result<CompactionResult, CompactionError>::err_value(
            compaction_error(CompactionErrorCode::InvalidSession,
                             "First kept entry has no UUID - session may need migration"));
    }

    std::string summary;
    if (preparation.isSplitTurn && !preparation.turnPrefixMessages.empty())
    {
        Result<std::string, CompactionError> history_result;
        if (!preparation.messagesToSummarize.empty())
        {
            history_result =
                generate_summary(preparation.messagesToSummarize, model,
                                 preparation.settings.reserveTokens, transport, apiKey, signal,
                                 customInstructions, preparation.previousSummary, thinkingLevel);
        }
        else
        {
            history_result =
                Result<std::string, CompactionError>::ok_value(std::string("No prior history."));
        }
        if (!history_result.ok)
        {
            return Result<CompactionResult, CompactionError>::err_value(history_result.error);
        }
        // turn prefix 摘要（简化：与主摘要共用生成函数）
        const int64_t max_tokens =
            static_cast<int64_t>(std::floor(0.5 * preparation.settings.reserveTokens));
        const std::string conversation_text =
            serialize_conversation(preparation.turnPrefixMessages);
        const std::string prompt_text = "<conversation>\n" + conversation_text +
                                        "\n</conversation>\n\n" + kTurnPrefixSummarizationPrompt;
        const auto prefix_result = generate_summary_internal(
            preparation.turnPrefixMessages, model, max_tokens, transport, apiKey, signal,
            prompt_text, conversation_text, thinkingLevel);
        if (!prefix_result.ok)
        {
            return Result<CompactionResult, CompactionError>::err_value(prefix_result.error);
        }
        summary = history_result.value + "\n\n---\n\n**Turn Context (split turn):**\n\n" +
                  prefix_result.value;
    }
    else
    {
        const auto summary_result = generate_summary(
            preparation.messagesToSummarize, model, preparation.settings.reserveTokens, transport,
            apiKey, signal, customInstructions, preparation.previousSummary, thinkingLevel);
        if (!summary_result.ok)
        {
            return Result<CompactionResult, CompactionError>::err_value(summary_result.error);
        }
        summary = summary_result.value;
    }

    const auto [read_files, modified_files] = compute_file_lists(preparation.fileOps);
    summary += format_file_operations(read_files, modified_files);

    CompactionResult result;
    result.summary = std::move(summary);
    result.firstKeptEntryId = preparation.firstKeptEntryId;
    result.tokensBefore = preparation.tokensBefore;
    result.details = Json{{"readFiles", read_files}, {"modifiedFiles", modified_files}};
    return Result<CompactionResult, CompactionError>::ok_value(std::move(result));
}

}  // namespace pi

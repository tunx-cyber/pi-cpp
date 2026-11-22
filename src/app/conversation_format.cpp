#include "pi/app/conversation_format.h"

namespace pi
{

namespace
{

std::string truncate_for_display(const std::string& text, size_t max_chars)
{
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars) + "\n...(truncated)";
}

}  // namespace

std::string format_conversation(const std::vector<AgentMessage>& messages, const Theme& theme)
{
    if (messages.empty()) return styled(theme, theme.dim, "(空会话)") + "\n";

    std::string out;
    for (const auto& message : messages)
    {
        if (message.role == Role::User)
        {
            std::string text = truncate_for_display(message.text_content(), kConversationMaxChars);
            bool has_image = false;
            for (const auto& block : message.content)
            {
                if (block.type == BlockType::Image) has_image = true;
            }
            if (text.empty())
            {
                if (!has_image) continue;
                text = "(图片)";
            }
            out += styled(theme, theme.user, "[you]") + "\n" + text + "\n\n";
        }
        else if (message.role == Role::Assistant)
        {
            const std::string text =
                truncate_for_display(message.text_content(), kConversationMaxChars);
            if (!text.empty())
            {
                out += styled(theme, theme.assistant, "[assistant]") + "\n" + text + "\n\n";
            }
            for (const auto& block : message.content)
            {
                if (block.type != BlockType::ToolCall) continue;
                out += styled(theme, theme.tool,
                              "[tool] " + block.name + " " +
                                  truncate_for_display(block.arguments.dump(), 300)) +
                       "\n";
            }
        }
        else if (message.role == Role::ToolResult)
        {
            const std::string text = truncate_for_display(message.text_content(), 500);
            if (text.empty()) continue;
            out += styled(theme, theme.dim, "[tool result] " + text) + "\n";
        }
    }
    return out;
}

}  // namespace pi

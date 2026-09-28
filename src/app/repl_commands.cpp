#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>

#include "pi/ai/model_registry.h"
#include "pi/app/commands.h"
#include "pi/app/images.h"
#include "pi/app/repl.h"
#include "pi/harness/prompt_templates.h"
#include "pi/harness/skills.h"

namespace pi
{
namespace
{
/** 内建 slash 命令表（/help 与命令菜单共用，保持单一来源；新增命令在此追加）。 */
const std::vector<CommandEntry>& builtin_command_entries()
{
    static const std::vector<CommandEntry> entries = {
        {"help", "显示帮助"},
        {"model", "查看或切换模型"},
        {"thinking", "查看或切换 thinking 级别"},
        {"compact", "手动压缩会话"},
        {"clear", "清空会话"},
        {"new", "新会话"},
        {"resume", "恢复会话（/resume 最近；/resume <序号|id> 指定）"},
        {"sessions", "列出历史会话（序号/时间/预览/消息数）"},
        {"image", "图片输入"},
        {"tools", "启用编码工具（read/bash/edit/write/grep/find/ls/web_fetch + subagent）"},
        {"skills", "列出 skills"},
        {"memory", "显示 picpp 进程当前及峰值内存"},
        {"quit", "退出"},
    };
    return entries;
}

/** ISO 时间 "2026-08-15T14:30:00.123Z" → "2026-08-15 14:30:00"。 */
std::string format_session_time(const std::string& iso)
{
    if (iso.size() < 19) return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 8);
}

/** 严格非负整数解析（空串/非数字/溢出返回 nullopt）。 */
std::optional<int> parse_uint(const std::string& s)
{
    if (s.empty()) return std::nullopt;
    int value = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9') return std::nullopt;
        if (value > (2147483647 - (c - '0')) / 10) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

/** 将支持的 thinking 级别列表格式化为 "off/high/xhigh"。 */
std::string join_thinking_levels(const std::vector<ThinkingLevel>& levels)
{
    std::string out;
    for (size_t i = 0; i < levels.size(); ++i)
    {
        if (i > 0) out += "/";
        out += to_string(levels[i]);
    }
    return out;
}

}  // namespace

// ---------- 命令菜单 ----------

bool Repl::command_menu_active() const
{
    const size_t first = line_buffer_.find_first_not_of(" \t");
    return first != std::string::npos && line_buffer_[first] == '/';
}

std::vector<CommandEntry> Repl::matching_commands() const
{
    std::vector<CommandEntry> matches;
    std::string typed = line_buffer_;
    const size_t first = typed.find_first_not_of(" \t");
    typed = first == std::string::npos ? "" : typed.substr(first + 1);  // 去掉前导空白与 /

    auto collect = [&](const std::vector<CommandEntry>& entries)
    {
        for (const auto& entry : entries)
        {
            if (typed.empty() || entry.name.compare(0, typed.size(), typed) == 0)
            {
                matches.push_back(entry);
            }
        }
    };
    collect(builtin_command_entries());
    collect(template_commands_);
    return matches;
}

void Repl::refresh_template_commands()
{
    template_commands_.clear();
    const auto templates =
        load_prompt_templates(fs_, {Settings::expand_home("~/.pi-cpp/templates")});
    for (const auto& template_ : templates.promptTemplates)
    {
        template_commands_.push_back({template_.name, template_.description});
    }
}

void Repl::complete_command()
{
    if (!command_menu_active()) return;
    const auto matches = matching_commands();
    if (matches.empty()) return;

    const size_t first = line_buffer_.find_first_not_of(" \t");
    const std::string typed = line_buffer_.substr(first + 1);

    if (matches.size() == 1)
    {
        line_buffer_ = line_buffer_.substr(0, first + 1) + matches[0].name + " ";
        cursor_ = line_buffer_.size();
        return;
    }
    // 多个匹配：补全到最长公共前缀
    std::string prefix = matches[0].name;
    for (size_t i = 1; i < matches.size(); ++i)
    {
        size_t k = 0;
        while (k < prefix.size() && k < matches[i].name.size() && prefix[k] == matches[i].name[k])
        {
            ++k;
        }
        prefix.resize(k);
    }
    if (prefix.size() > typed.size())
    {
        line_buffer_ = line_buffer_.substr(0, first + 1) + prefix;
        cursor_ = line_buffer_.size();
    }
}

// ---------- 命令分发 ----------

void Repl::handle_command(const std::string& line)
{
    const size_t space = line.find_first_of(" \t");
    const std::string name =
        line.substr(1, space == std::string::npos ? std::string::npos : space - 1);

    if (name == "help")
    {
        std::cout << help_text() << std::endl;
        return;
    }
    if (name == "model")
    {
        if (space != std::string::npos)
        {
            const std::string model_id = line.substr(space + 1);
            if (get_model(model_id))
            {
                const auto old_thinking = session_.thinking_level();
                session_.set_model(model_id);
                std::cout << "model → " << model_id << std::endl;
                if (session_.thinking_level() != old_thinking)
                {
                    std::cout << "（thinking 已自动回退：" << to_string(old_thinking) << " → "
                              << to_string(session_.thinking_level()) << "）" << std::endl;
                }
            }
            else
            {
                std::cout << "未知模型：" << model_id << std::endl;
            }
        }
        else
        {
            std::cout << "当前模型：" << session_.model().id << std::endl;
            std::vector<std::string> ids;
            for (const auto& provider : get_providers())
            {
                for (const auto& m : get_models(provider)) ids.push_back(m.id);
            }
            std::cout << "可用模型：";
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (i) std::cout << ", ";
                std::cout << ids[i];
            }
            std::cout << std::endl;
        }
        return;
    }
    if (name == "thinking")
    {
        if (space != std::string::npos)
        {
            const auto level = thinking_level_from_string(line.substr(space + 1));
            if (level)
            {
                // 用 clamp_thinking_level 将请求回退到当前模型真正支持的最近级别，
                // 避免把 deepseek 不认识的 low/medium 等原样发给 API。
                const auto clamped = clamp_thinking_level(session_.model(), *level);
                session_.set_thinking(clamped);
                if (clamped != *level)
                {
                    std::cout << "thinking → " << to_string(clamped) << "（" << to_string(*level)
                              << " 当前模型不支持，已自动回退）" << std::endl;
                }
                else
                {
                    std::cout << "thinking → " << to_string(clamped) << std::endl;
                }
            }
            else
            {
                std::cout << "取值：off/minimal/low/medium/high/xhigh；当前模型 "
                          << session_.model().id << " 支持："
                          << join_thinking_levels(get_supported_thinking_levels(session_.model()))
                          << std::endl;
            }
        }
        else
        {
            std::cout << "当前 thinking：" << to_string(session_.thinking_level()) << std::endl;
        }
        return;
    }
    if (name == "compact")
    {
        // 压缩需要一次真实的 LLM 摘要调用（在 UI 线程上同步执行），先给出提示
        std::cout << "压缩中…" << std::flush;
        std::string error;
        if (session_.compact(&error))
        {
            std::cout << "已压缩会话" << std::endl;
        }
        else
        {
            std::cout << "压缩失败：" << error << std::endl;
        }
        return;
    }
    if (name == "clear")
    {
        session_.reset();
        std::cout << "会话已清空" << std::endl;
        return;
    }
    if (name == "resume")
    {
        std::string arg;
        if (space != std::string::npos)
        {
            const size_t a = line.find_first_not_of(" \t", space + 1);
            if (a != std::string::npos) arg = line.substr(a);
        }
        if (arg.empty())
        {
            session_.resume();
            std::cout << "已恢复最近会话（" << session_.messages().size() << " 条消息）"
                      << std::endl;
        }
        else
        {
            std::string error;
            bool ok = false;
            if (const auto idx = parse_uint(arg))
            {
                ok = session_.resume_by_index(*idx, &error);
            }
            else
            {
                ok = session_.resume_by_id(arg, &error);
            }
            if (ok)
            {
                std::cout << "已恢复会话（" << session_.messages().size() << " 条消息）"
                          << std::endl;
            }
            else
            {
                std::cout << "恢复失败：" << error << std::endl;
            }
        }
        return;
    }
    if (name == "new")
    {
        session_.new_session();
        std::cout << "已创建新会话" << std::endl;
        return;
    }
    if (name == "sessions")
    {
        const auto sessions = session_.list_sessions();
        if (sessions.empty())
        {
            std::cout << "没有会话（当前目录 " << cwd_ << "）" << std::endl;
            return;
        }
        const std::string current = session_.current_session_id();
        std::cout << "会话（" << sessions.size() << "，当前目录 " << cwd_ << "）：" << std::endl;
        std::cout << "  /resume <序号|id> 恢复到指定会话" << std::endl;
        for (size_t i = 0; i < sessions.size(); ++i)
        {
            const auto& s = sessions[i];
            const bool is_current = (s.id == current);
            std::cout << "  [" << i << "] " << format_session_time(s.createdAt);
            if (!s.preview.empty()) std::cout << "  \"" << s.preview << "\"";
            std::cout << "  (" << s.messageCount << " 条消息)  id=" << s.id.substr(0, 8);
            if (is_current) std::cout << "  ← 当前";
            std::cout << std::endl;
        }
        return;
    }
    if (name == "skills")
    {
        const auto skills = load_skills(fs_, {Settings::expand_home("~/.pi-cpp/skills")});
        if (skills.skills.empty())
        {
            std::cout << "无 skills（~/.pi-cpp/skills/）" << std::endl;
        }
        else
        {
            for (const auto& skill : skills.skills)
            {
                std::cout << "  " << skill.name << " - " << skill.description << std::endl;
            }
        }
        return;
    }
    if (name == "memory")
    {
        const ProcessMemoryUsage current = sample_memory();
        std::cout << "Memory: " << format_process_memory(current);
        if (peak_memory_.hasResident)
        {
            std::cout << " (peak " << format_memory_bytes(peak_memory_.residentBytes) << ")";
        }
        std::cout << std::endl;
        return;
    }
    if (name == "quit")
    {
        quit_requested_ = true;
        return;
    }
    if (name == "image")
    {
        const std::string path = line.substr(space + 1);
        const auto block = load_image_as_block(path);
        if (block)
        {
            std::cout << "已加载图片（" << block->data.size() / 1024 << " KB base64）" << std::endl;
            session_.prompt("", {*block});
        }
        else
        {
            std::cout << "无法加载图片：" << path << std::endl;
        }
        return;
    }
    if (name == "tools")
    {
        session_.set_tools(
            make_coding_tools(cwd_, session_.api_key(), session_.web_search_config()));
        std::cout << "已启用编码工具：read/bash/edit/write/grep/find/ls/web_fetch/web_search"
                  << std::endl;
        return;
    }

    // 用户模板：~/.pi-cpp/templates/<name>.md → /<name> args
    // （菜单只缓存名称与描述，模板正文在提交时重新读取）
    const auto templates =
        load_prompt_templates(fs_, {Settings::expand_home("~/.pi-cpp/templates")});
    for (const auto& template_ : templates.promptTemplates)
    {
        if (template_.name == name)
        {
            const auto args =
                parse_command_args(space == std::string::npos ? "" : line.substr(space + 1));
            start_run(substitute_args(template_.content, args));
            return;
        }
    }
    std::cout << "未知命令 /" << name << "（/help 查看）" << std::endl;
}

std::string Repl::help_text()
{
    std::string out = "命令（输入 / 唤起菜单，Tab 补全）：\n";
    for (const auto& entry : builtin_command_entries())
    {
        out += "  /" + entry.name + " - " + entry.description + "\n";
    }
    out += "\n用户模板：~/.pi-cpp/templates/<name>.md → /<name> 参数\n";
    out += "输入：Enter=提交  Shift+Enter/Ctrl+J/Option+Enter=换行\n";
    out += "      （Shift+Enter 需终端支持 CSI-u/kitty 键盘协议：iTerm2/kitty/WezTerm）\n";
    out += "流式中：Enter=steer Esc=abort Ctrl+C=退出（确认）Ctrl+P=切模型 Ctrl+L=重绘\n";
    return out;
}

void Repl::cycle_model()
{
    std::vector<std::string> ids;
    for (const auto& provider : get_providers())
    {
        for (const auto& m : get_models(provider)) ids.push_back(m.id);
    }
    if (ids.empty()) return;
    const std::string current = session_.model().id;
    const auto old_thinking = session_.thinking_level();
    const auto it = std::find(ids.begin(), ids.end(), current);
    const size_t next =
        it == ids.end() ? 0
                        : (static_cast<size_t>(std::distance(ids.begin(), it)) + 1) % ids.size();
    session_.set_model(ids[next]);
    std::cout << "\r\nmodel → " << session_.model().id << std::endl;
    // set_model 内部已保证 thinking 合法；若发生了回退则明确提示（与 /model 命令一致）
    if (session_.thinking_level() != old_thinking)
    {
        std::cout << "（thinking 已自动回退：" << to_string(old_thinking) << " → "
                  << to_string(session_.thinking_level()) << "）" << std::endl;
    }
}

}  // namespace pi

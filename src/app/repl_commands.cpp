#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>

#include "pi/ai/model_registry.h"
#include "pi/app/commands.h"
#include "pi/app/images.h"
#include "pi/app/repl.h"
#include "pi/app/selection.h"
#include "pi/app/terminal_input.h"
#include "pi/app/usage_format.h"
#include "pi/harness/compaction.h"
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
        {"model", "查看或切换模型（↑/↓ 选择）"},
        {"thinking", "查看或切换 thinking 级别（↑/↓ 选择）"},
        {"compact", "手动压缩会话"},
        {"clear", "清空会话"},
        {"new", "新会话"},
        {"resume", "恢复会话（/resume 最近；/resume <序号|id> 指定）"},
        {"sessions", "列出历史会话（序号/时间/预览/消息数）"},
        {"image", "图片输入"},
        {"tools", "重新加载编码工具（启动时已默认启用）"},
        {"skills", "列出 skills"},
        {"memory", "显示 picpp 进程当前及峰值内存"},
        {"cost", "显示四项 token 用量（价格会变动，不换算金额）"},
        {"context", "显示当前上下文的 token 估算"},
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

/** 注册表（内置 + settings.json 自定义）里的全部模型 id。 */
std::vector<std::string> registered_model_ids()
{
    std::vector<std::string> ids;
    for (const auto& provider : get_providers())
    {
        for (const auto& model : get_models(provider)) ids.push_back(model.id);
    }
    return ids;
}

/** 全部 thinking 级别（选择器与错误提示共用）。 */
const std::vector<ThinkingLevel>& all_thinking_levels()
{
    static const std::vector<ThinkingLevel> levels = {
        ThinkingLevel::Off,    ThinkingLevel::Minimal, ThinkingLevel::Low,
        ThinkingLevel::Medium, ThinkingLevel::High,    ThinkingLevel::Xhigh,
    };
    return levels;
}

/** 将 thinking 级别列表格式化为 "off/high/xhigh"。 */
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

std::vector<std::string> Repl::command_menu_lines() const
{
    std::vector<std::string> lines;
    const auto entries = matching_commands();
    if (entries.empty()) return lines;

    const size_t selected = clamp_selection(menu_index_, entries.size());
    const auto window = selection_window(entries.size(), selected, kMenuMaxLines);

    size_t max_name = 0;
    for (size_t i = window.offset; i < window.offset + window.visible; ++i)
    {
        max_name = std::max(max_name, entries[i].name.size());
    }
    for (size_t i = window.offset; i < window.offset + window.visible; ++i)
    {
        const bool active = (i == selected);
        std::string line = active ? "> /" : "  /";
        line += entries[i].name;
        line.append(max_name - entries[i].name.size() + 2, ' ');
        line += entries[i].description;
        // 反显选中行（终端不支持时也只是没有高亮，不影响可读性）
        if (active) line = "\033[7m" + line + "\033[0m";
        lines.push_back(std::move(line));
    }

    const size_t hidden = entries.size() - window.visible;
    if (hidden > 0)
    {
        lines.push_back("  ... (+" + std::to_string(hidden) + " more)");
    }
    return lines;
}

void Repl::move_menu_selection(int delta)
{
    const size_t total = matching_commands().size();
    if (total == 0) return;
    const size_t current = clamp_selection(menu_index_, total);
    if (delta < 0)
    {
        menu_index_ = current == 0 ? 0 : current - 1;
    }
    else
    {
        menu_index_ = std::min(current + 1, total - 1);
    }
}

void Repl::accept_menu_selection()
{
    const auto matches = matching_commands();
    if (matches.empty()) return;
    const size_t selected = clamp_selection(menu_index_, matches.size());

    const size_t first = line_buffer_.find_first_not_of(" \t");
    const std::string leading = first == std::string::npos ? "" : line_buffer_.substr(0, first);
    // 末尾留空格：命令需要参数时可直接继续输入（无参数命令也兼容，分发只取命令名）
    line_buffer_ = leading + "/" + matches[selected].name + " ";
    cursor_ = line_buffer_.size();
    menu_index_ = selected;
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
    // Tab 只把当前高亮项补全到输入缓冲区（不执行）；↑/↓ 决定高亮项。
    if (!command_menu_active()) return;
    accept_menu_selection();
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
            const size_t arg_start = line.find_first_not_of(" \t", space + 1);
            if (arg_start != std::string::npos)
            {
                const std::string model_id = line.substr(arg_start);
                // 注册表（内置元数据 + settings.json 自定义）优先，其次以端点 GET /models 为准。
                bool verified = get_model(model_id).has_value();
                std::string warning;
                if (!verified)
                {
                    const bool remote_ok = load_remote_models(true);
                    verified =
                        remote_ok && std::find(remote_model_ids_.begin(), remote_model_ids_.end(),
                                               model_id) != remote_model_ids_.end();
                    if (!remote_ok)
                    {
                        // 端点不可用时无法验证：仍允许直接指定，避免离线/代理场景被卡死。
                        warning =
                            "无法验证模型 id（GET /models 失败：" + remote_models_error_ + "）";
                    }
                    else if (!verified)
                    {
                        std::cout << "未知模型：" << model_id << "（/model 查看可用列表）"
                                  << std::endl;
                        return;
                    }
                }
                if (!warning.empty()) std::cout << warning << std::endl;
                std::cout << apply_model(model_id) << std::endl;
                return;
            }
        }
        // 无参数：打开交互式选择器（↑/↓ 选择，Enter 确认，Esc 取消）
        open_model_picker();
        return;
    }
    if (name == "thinking")
    {
        if (space != std::string::npos)
        {
            const size_t arg_start = line.find_first_not_of(" \t", space + 1);
            if (arg_start != std::string::npos)
            {
                const std::string level_name = line.substr(arg_start);
                if (!thinking_level_from_string(level_name))
                {
                    std::cout << "未知 thinking 级别：" << level_name << "（取值："
                              << join_thinking_levels(all_thinking_levels()) << "）" << std::endl;
                    return;
                }
                std::cout << apply_thinking(level_name) << std::endl;
                return;
            }
        }
        // 无参数：打开上下键选择器
        open_thinking_picker();
        return;
    }
    if (name == "compact")
    {
        // 摘要是一次真实的 LLM 调用：放到 worker 线程，UI 不冻结，Esc 可取消。
        start_compact();
        return;
    }
    if (name == "clear")
    {
        // 只清内存的话，磁盘上的 JSONL 仍在，下次 /resume 会把旧历史带回来；
        // 因此直接开一个新会话文件（与 /new 同路径，旧文件保留可查）。
        session_.new_session();
        std::cout << "已清空（新会话 " << session_.current_session_id().substr(0, 8) << "）"
                  << std::endl;
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
            print_conversation();
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
                print_conversation();
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
    if (name == "cost")
    {
        // 价格不断变动，因此只展示累计的 token 数量，不做金额换算。
        std::cout << "Token 用量（仅统计 token，不显示金额）：" << std::endl;
        std::cout << "  当前会话  " << format_token_usage(session_.session_usage()) << std::endl;
        const Usage last_turn = session_.last_turn_usage();
        if (last_turn.totalTokens > 0)
        {
            std::cout << "  最近一次  " << format_token_usage(last_turn) << std::endl;
        }
        std::cout << "  进程累计  " << format_token_usage(session_.all_time_usage()) << std::endl;
        return;
    }
    if (name == "context")
    {
        const auto messages = session_.messages();
        const auto estimate = estimate_context_tokens(messages);
        const auto model = session_.model();
        std::cout << "上下文估算：" << format_token_count(estimate.tokens) << " tokens（"
                  << messages.size() << " 条消息）" << std::endl;
        if (estimate.usageTokens > 0)
        {
            std::cout << "  最近一次调用 " << format_token_count(estimate.usageTokens)
                      << " + 之后新增 " << format_token_count(estimate.trailingTokens) << std::endl;
        }
        if (model.contextWindow > 0)
        {
            const int64_t percent = estimate.tokens * 100 / model.contextWindow;
            std::cout << "  模型上限 " << format_token_count(model.contextWindow) << "（占用 "
                      << percent << "%）" << std::endl;
        }
        std::cout << "  估算口径：字符数/4，图片按 4800 tokens（不是精确计数）" << std::endl;
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
    std::string out = "命令（输入 / 唤起菜单：↑/↓ 选择，Tab 补全，Enter 执行）：\n";
    for (const auto& entry : builtin_command_entries())
    {
        out += "  /" + entry.name + " - " + entry.description + "\n";
    }
    out += "/model、/thinking 无参数时打开选择器：↑/↓ 选择，Enter 确认，Esc 取消\n";
    out += "/context 显示当前上下文 token 估算；/compact 手动压缩上下文（不再自动压缩）\n";
    out += "默认模型：未配置时取 GET /models 返回的第一个模型（不写死）\n";
    out += "\n用户模板：~/.pi-cpp/templates/<name>.md → /<name> 参数\n";
    out += "输入：Enter=提交  Shift+Enter/Ctrl+J/Option+Enter=换行\n";
    out += "      （Shift+Enter 需终端支持 CSI-u/kitty 键盘协议：iTerm2/kitty/WezTerm）\n";
    out += "流式中：Enter=steer Esc=abort Ctrl+C=退出（确认）Ctrl+P=切模型 Ctrl+L=重绘\n";
    return out;
}

// ---------- 选择器（模型 / thinking 共用） ----------

std::vector<std::string> Repl::picker_lines() const
{
    std::vector<std::string> lines;
    const size_t total = picker_items_.size();
    if (total == 0) return lines;

    const size_t selected = clamp_selection(picker_index_, total);
    const auto window = selection_window(total, selected, kMenuMaxLines);
    const std::string current = picker_kind_ == PickerKind::Model
                                    ? session_.model().id
                                    : to_string(session_.thinking_level());

    lines.push_back(picker_title_);

    for (size_t i = window.offset; i < window.offset + window.visible; ++i)
    {
        const bool active = (i == selected);
        std::string line = active ? "> " : "  ";
        line += (picker_items_[i] == current) ? "* " : "  ";
        line += picker_items_[i];
        // 模型：注册表有元数据时补上可读名称（远端列表只有 id）
        if (picker_kind_ == PickerKind::Model)
        {
            if (const auto info = get_model(picker_items_[i]);
                info && !info->name.empty() && info->name != info->id)
            {
                line += "  (" + info->name + ")";
            }
        }
        // 反显选中行（终端不支持时也只是没有高亮）
        if (active) line = "\033[7m" + line + "\033[0m";
        lines.push_back(std::move(line));
    }
    const size_t hidden = total - window.visible;
    if (hidden > 0) lines.push_back("  ... (+" + std::to_string(hidden) + " more)");
    return lines;
}

void Repl::handle_picker_key(const terminal::KeyEvent& key)
{
    switch (key.type)
    {
        case terminal::KeyEvent::Type::Up:
            if (picker_index_ > 0) --picker_index_;
            break;
        case terminal::KeyEvent::Type::Down:
            if (picker_index_ + 1 < picker_items_.size()) ++picker_index_;
            break;
        case terminal::KeyEvent::Type::Enter:
            apply_picker_selection();
            break;
        case terminal::KeyEvent::Type::Esc:
        case terminal::KeyEvent::Type::CtrlC:  // 选择器内 Ctrl+C 只关闭选择器，不退出进程
            close_picker("已取消选择");
            break;
        default:
            break;  // 忽略其他按键，避免选择过程中误改输入缓冲区
    }
}

void Repl::open_picker(PickerKind kind, std::vector<std::string> items, std::string title)
{
    if (items.empty())
    {
        std::cout << "没有可选项" << std::endl;
        return;
    }
    picker_kind_ = kind;
    picker_items_ = std::move(items);
    picker_title_ = std::move(title);
    picker_index_ = 0;
    const std::string current =
        kind == PickerKind::Model ? session_.model().id : to_string(session_.thinking_level());
    for (size_t i = 0; i < picker_items_.size(); ++i)
    {
        if (picker_items_[i] == current)
        {
            picker_index_ = i;
            break;
        }
    }
    picker_active_ = true;
}

void Repl::open_model_picker()
{
    std::string endpoint = session_.base_url();
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();

    // /models 是同步请求（最长 10s），先给出提示；完成后擦掉该提示再渲染选择器。
    std::cout << "正在获取模型列表（GET " << endpoint << "/models）…" << std::flush;
    const bool remote_ok = load_remote_models(true);
    std::cout << "\r\033[2K" << std::flush;

    if (!remote_ok)
    {
        std::cout << "无法获取模型列表：" << remote_models_error_ << "\n"
                  << "可用 /model <id> 直接指定模型 id" << std::endl;
        return;
    }
    if (remote_model_ids_.empty())
    {
        std::cout << "端点未返回任何模型（GET " << endpoint << "/models）\n"
                  << "可用 /model <id> 直接指定模型 id" << std::endl;
        return;
    }

    open_picker(PickerKind::Model, remote_model_ids_,
                "模型列表（" + std::to_string(remote_model_ids_.size()) +
                    "）：↑/↓ 选择  Enter 确认  Esc 取消");
}

void Repl::open_thinking_picker()
{
    std::vector<std::string> levels;
    levels.reserve(all_thinking_levels().size());
    for (const auto level : all_thinking_levels()) levels.push_back(to_string(level));
    open_picker(PickerKind::Thinking, std::move(levels),
                "thinking（↑/↓ 选择  Enter 确认  Esc 取消；不做模型能力校验）");
}

void Repl::close_picker(const std::string& message)
{
    picker_active_ = false;
    picker_items_.clear();
    picker_title_.clear();
    print_above_prompt(message);
}

void Repl::apply_picker_selection()
{
    if (picker_items_.empty())
    {
        close_picker("没有可选项");
        return;
    }
    const std::string item = picker_items_[clamp_selection(picker_index_, picker_items_.size())];
    const PickerKind kind = picker_kind_;
    picker_active_ = false;
    picker_items_.clear();
    picker_title_.clear();
    print_above_prompt(kind == PickerKind::Thinking ? apply_thinking(item) : apply_model(item));
}

std::string Repl::apply_model(const std::string& model_id)
{
    session_.set_model(model_id);
    return "model → " + session_.model().id;
}

std::string Repl::apply_thinking(const std::string& level_name)
{
    const auto level = thinking_level_from_string(level_name);
    if (!level) return "未知 thinking 级别：" + level_name;
    session_.set_thinking(*level);
    return std::string("thinking → ") + to_string(*level);
}

bool Repl::load_remote_models(bool force)
{
    if (remote_models_loaded_ && !force) return remote_models_error_.empty();
    const RemoteModelList list = session_.list_models();
    remote_models_loaded_ = true;
    remote_models_error_ = list.error;
    remote_model_ids_.clear();
    if (list.error.empty())
    {
        remote_model_ids_.reserve(list.models.size());
        for (const auto& model : list.models) remote_model_ids_.push_back(model.id);
    }
    return list.error.empty();
}

std::vector<std::string> Repl::available_model_ids()
{
    // 远端列表优先；首次 Ctrl+P 时拉取一次，失败则回退内置/自定义模型表。
    if (load_remote_models(false) && !remote_model_ids_.empty()) return remote_model_ids_;
    return registered_model_ids();
}

void Repl::cycle_model()
{
    std::vector<std::string> ids = available_model_ids();
    if (ids.empty())
    {
        std::cout << "\r\n没有可用模型（/model 查看）" << std::endl;
        return;
    }
    const std::string current = session_.model().id;
    const auto it = std::find(ids.begin(), ids.end(), current);
    const size_t next =
        it == ids.end() ? 0
                        : (static_cast<size_t>(std::distance(ids.begin(), it)) + 1) % ids.size();
    std::cout << "\r\n" << apply_model(ids[next]) << std::endl;
}

}  // namespace pi

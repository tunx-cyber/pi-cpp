#include "pi/app/repl.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cstdlib>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

#include "pi/ai/model_registry.h"
#include "pi/app/commands.h"
#include "pi/app/images.h"
#include "pi/harness/prompt_templates.h"
#include "pi/harness/skills.h"

namespace pi
{

namespace
{

constexpr const char* kBanner =
    "pi-cpp — C++ terminal agent (pi 移植版)\n"
    "输入 / 唤起命令菜单（Tab 补全）。Enter=提交  Shift+Enter/Ctrl+J/Option+Enter=换行\n"
    "流式中：Enter=steer Esc=abort Ctrl+C=退出（确认）Ctrl+P=切模型 Ctrl+L=重绘\n";

/** 命令菜单最多显示的条目数。 */
constexpr size_t kMenuMaxLines = 8;

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

std::string status_text(const AgentSession& session, const ProcessMemoryUsage& memory,
                        const ProcessMemoryUsage& peak_memory)
{
    const ModelInfo model = session.model();
    std::string thinking = to_string(session.thinking_level());
    std::string out = "[model=" + model.id + "] [thinking=" + thinking + "]";
    out += " [turn=¥" + std::to_string(session.total_cost()) + "]";
    out += " [all=¥" + std::to_string(session.all_time_cost()) + "]";
    out += " [mem " + format_process_memory(memory);
    if (peak_memory.hasResident)
    {
        out += " peak=" + format_memory_bytes(peak_memory.residentBytes);
    }
    out += "]";
    return out;
}

/** 格式化 usage 为 tokens: in=N out=M cache=K 一行。 */
std::string format_usage_line(const Usage& usage)
{
    std::string line = "Tokens: in=" + std::to_string(usage.input) +
                       " out=" + std::to_string(usage.output) +
                       " cache=" + std::to_string(usage.cacheRead + usage.cacheWrite);
    if (usage.cacheRead > 0) line += " (read=" + std::to_string(usage.cacheRead) + ")";
    if (usage.cacheWrite > 0) line += " (write=" + std::to_string(usage.cacheWrite) + ")";
    return line;
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

// ---------- UTF-8 与终端显示宽度 ----------

/** UTF-8 首字节对应的后续字节数（非法首字节返回 0）。 */
int utf8_continuation_bytes(unsigned char lead)
{
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;
    return 0;
}

/** Unicode 码点 → UTF-8 字节序列（CSI-u/kitty 协议上报非 ASCII 键时使用）。 */
std::string utf8_encode(uint32_t codepoint)
{
    std::string out;
    if (codepoint < 0x80)
    {
        out += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800)
    {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000)
    {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return out;
}

/** 近似 wcwidth 的宽字符判断（CJK 主区间 + 全角符号 + 常用 emoji，不依赖 locale）。 */
bool is_wide_codepoint(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||   // CJK 部首/汉字/假名/谚文
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul 音节
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容表意
           (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK 兼容形式
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角形式
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // 全角符号
           (cp >= 0x1F300 && cp <= 0x1FAFF) || // 表情符号/符号
           (cp >= 0x20000 && cp <= 0x3FFFD);   // CJK 扩展 B+
}

/** 字符串的终端显示宽度（宽字符计 2 列），用于多行光标定位。 */
size_t display_width(const std::string& text)
{
    size_t width = 0;
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80)
        {
            width += 1;
            ++i;
            continue;
        }
        const int extra = utf8_continuation_bytes(c);
        if (extra <= 0 || i + extra >= text.size())
        {
            width += 1;  // 非法序列按 1 列兜底
            ++i;
            continue;
        }
        uint32_t cp = c & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k)
        {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        width += is_wide_codepoint(cp) ? 2 : 1;
        i += extra + 1;
    }
    return width;
}

size_t count_newlines(const std::string& text)
{
    size_t count = 0;
    for (char c : text)
    {
        if (c == '\n') ++count;
    }
    return count;
}

// ---------- 键盘输入解码 ----------

/** 带超时的单字节读取。raw mode 下裸按 Esc 只有 1 字节，直接阻塞读后续字节会挂起 UI。 */
bool read_byte_with_timeout(char& out, int timeout_ms)
{
    struct pollfd pfd
    {
        STDIN_FILENO, POLLIN, 0
    };
    if (poll(&pfd, 1, timeout_ms) <= 0) return false;
    return read(STDIN_FILENO, &out, 1) == 1;
}

/** stdin 在指定时间内是否有数据（用于流式中 Ctrl+C 二次确认）。 */
bool stdin_has_data(int timeout_ms)
{
    struct pollfd pfd
    {
        STDIN_FILENO, POLLIN, 0
    };
    return poll(&pfd, 1, timeout_ms) > 0;
}

/**
 * 解码后的按键事件。兼容普通终端字节流（CR/BS/ESC [ 序列）与
 * CSI-u/kitty 键盘协议（`CSI <codepoint> [;<modifiers>] u`，enter_raw_mode 时
 * 通过 `CSI > 1 u` 启用；不支持的终端忽略启用序列、继续发传统字节流）。
 */
struct KeyEvent
{
    enum class Type
    {
        None,
        Char,        // text 为待插入的字节序列
        Enter,
        ShiftEnter,  // 换行：Shift+Enter（CSI-u）/ Ctrl+J / Option+Enter
        Esc,
        CtrlC,
        CtrlL,
        CtrlP,
        Tab,
        Backspace,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
    };
    Type type = Type::None;
    std::string text;
};

KeyEvent arrow_key_event(char final_byte)
{
    KeyEvent key;
    switch (final_byte)
    {
        case 'A': key.type = KeyEvent::Type::Up; break;
        case 'B': key.type = KeyEvent::Type::Down; break;
        case 'C': key.type = KeyEvent::Type::Right; break;
        case 'D': key.type = KeyEvent::Type::Left; break;
        case 'H': key.type = KeyEvent::Type::Home; break;
        case 'F': key.type = KeyEvent::Type::End; break;
        default: break;
    }
    return key;
}

/** 解析 CSI-u 序列（形如 "13;1u"，不带前导 ESC [）。 */
KeyEvent decode_csi_u_sequence(const std::string& seq)
{
    KeyEvent key;
    const size_t semi = seq.find(';');
    const int code = std::atoi(seq.substr(0, semi).c_str());
    const int mods = semi == std::string::npos ? 0 : std::atoi(seq.substr(semi + 1).c_str());

    switch (code)
    {
        case 13:
            // kitty 协议 shift=1、alt=2；iTerm2 传统 CSI-u 用 2 表示 shift。
            // 两者都映射为换行（Alt+Enter 插换行同样是合理行为）。
            key.type = (mods & 3) ? KeyEvent::Type::ShiftEnter : KeyEvent::Type::Enter;
            return key;
        case 27: key.type = KeyEvent::Type::Esc; return key;
        case 9: key.type = KeyEvent::Type::Tab; return key;
        case 127: key.type = KeyEvent::Type::Backspace; return key;
        case 65: key.type = KeyEvent::Type::Up; return key;
        case 66: key.type = KeyEvent::Type::Down; return key;
        case 67: key.type = KeyEvent::Type::Right; return key;
        case 68: key.type = KeyEvent::Type::Left; return key;
        default: break;
    }
    if (mods & 4)  // Ctrl 修饰
    {
        if (code == 99 || code == 3) key.type = KeyEvent::Type::CtrlC;    // c
        else if (code == 108) key.type = KeyEvent::Type::CtrlL;           // l
        else if (code == 112) key.type = KeyEvent::Type::CtrlP;           // p
        else if (code == 106) key.type = KeyEvent::Type::ShiftEnter;      // Ctrl+J 换行兜底
        return key;
    }
    if (code >= 32)
    {
        key.type = KeyEvent::Type::Char;
        key.text = utf8_encode(static_cast<uint32_t>(code));
    }
    return key;
}

KeyEvent decode_key_event()
{
    KeyEvent key;
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return key;

    const auto char_event = [&](std::string text)
    {
        key.type = KeyEvent::Type::Char;
        key.text = std::move(text);
        return key;
    };

    switch (static_cast<unsigned char>(c))
    {
        case '\r':
        case '\n':  // 部分终端在 raw mode 下仍因 ICRNL 把 Enter 送到为 \n
            key.type = KeyEvent::Type::Enter;
            return key;
        case 3: key.type = KeyEvent::Type::CtrlC; return key;
        case 12: key.type = KeyEvent::Type::CtrlL; return key;
        case 16: key.type = KeyEvent::Type::CtrlP; return key;
        case 9: key.type = KeyEvent::Type::Tab; return key;
        case 127:
        case 8: key.type = KeyEvent::Type::Backspace; return key;
        case 27:
        {
            // Esc：可能是裸 Esc、SS3（Fn 方向键）、CSI 序列或 CSI-u/kitty 协议
            char next;
            if (!read_byte_with_timeout(next, 50))
            {
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (next == '\r' || next == '\n')
            {
                key.type = KeyEvent::Type::ShiftEnter;  // Option+Enter：全终端可用的换行兜底
                return key;
            }
            if (next == 'O')  // SS3
            {
                char arrow;
                if (read_byte_with_timeout(arrow, 50)) return arrow_key_event(arrow);
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (next != '[')
            {
                return char_event(std::string(1, next));  // Alt+字符（忽略修饰）
            }
            // CSI 序列：收集到终止字节（字母 / ~）
            std::string seq;
            while (seq.size() < 24)
            {
                char b;
                if (!read_byte_with_timeout(b, 50)) break;
                seq += b;
                if (b == 'u' || (b >= 'A' && b <= 'Z') || b == '~') break;
            }
            if (seq.empty())
            {
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (seq.back() == 'u') return decode_csi_u_sequence(seq);
            return arrow_key_event(seq.back());  // 传统 CSI：A/B/C/D/H/F，其余忽略
        }
        default:
        {
            // UTF-8 多字节序列：读取完整字符
            std::string text(1, c);
            const int extra = utf8_continuation_bytes(static_cast<unsigned char>(c));
            for (int i = 0; i < extra; ++i)
            {
                char b;
                if (!read_byte_with_timeout(b, 50)) break;
                text += b;
            }
            return char_event(std::move(text));
        }
    }
}

}  // namespace

Repl::Repl(AgentSession& session, std::string cwd)
    : session_(session), cwd_(std::move(cwd)), fs_(cwd_)
{
    pipe(event_pipe_);
    fcntl(event_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(event_pipe_[1], F_SETFL, O_NONBLOCK);

    // 单一订阅（构造时注册一次，避免每次 run 累积订阅导致事件重复处理）
    session_.subscribe(
        [this](const AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
        {
            if (event.type == AgentEvent::Type::MessageUpdate && event.assistantMessageEvent)
            {
                const auto& stream_event = *event.assistantMessageEvent;
                if (stream_event.type == StreamEvent::Type::TextDelta ||
                    stream_event.type == StreamEvent::Type::ThinkingDelta)
                {
                    enqueue(UiEvent{UiEvent::Type::Delta, stream_event.delta, "", ""});
                }
            }
            if (event.type == AgentEvent::Type::ToolExecutionStart)
            {
                // 提取工具特有信息用于展示
                std::string detail;
                if (event.toolName == "bash" && event.args.contains("command") &&
                    event.args["command"].is_string())
                {
                    detail = event.args["command"].get<std::string>();
                }
                else if (event.toolName == "edit" && event.args.contains("path") &&
                         event.args["path"].is_string())
                {
                    detail = event.args["path"].get<std::string>();
                }
                enqueue(UiEvent{UiEvent::Type::ToolStart, "", event.toolName, std::move(detail)});
            }
            if (event.type == AgentEvent::Type::ToolExecutionEnd)
            {
                // 收集工具输出的文本部分；edit 工具生成 diff 格式
                std::string detail;
                if (event.toolName == "edit")
                {
                    // LLM 可能传非字符串参数；用 value() 兜底，避免 get<string>() 抛异常
                    const std::string old_str = event.args.value("oldString", std::string(""));
                    const std::string new_str = event.args.value("newString", std::string(""));
                    // 格式：每行前加 -（删除）/ +（新增），左侧空白填充对齐
                    auto format_diff = [](const std::string& s, const char* prefix)
                    {
                        std::string out;
                        std::istringstream stream(s);
                        std::string line;
                        while (std::getline(stream, line)) out += std::string(prefix) + line + "\n";
                        if (s.empty() || s.back() == '\n') out += std::string(prefix) + "\n";
                        return out;
                    };
                    detail = format_diff(old_str, "- ") + format_diff(new_str, "+ ");
                    // 去掉末尾多余换行
                    while (!detail.empty() && detail.back() == '\n') detail.pop_back();
                }
                else
                {
                    for (const auto& block : event.result.content)
                    {
                        if (block.type == BlockType::Text) detail += block.text;
                    }
                }
                enqueue(UiEvent{UiEvent::Type::ToolEnd, "", event.toolName, std::move(detail)});
            }
            if (event.type == AgentEvent::Type::MessageEnd && event.message.role == Role::Assistant)
            {
                if (event.message.stopReason == StopReason::Error)
                {
                    const std::string message = event.message.errorMessage.empty()
                                                    ? "AI request failed"
                                                    : event.message.errorMessage;
                    enqueue(UiEvent{UiEvent::Type::Delta, "\r\n[error] " + message + "\n", "", ""});
                }
                else if (event.message.stopReason == StopReason::Aborted)
                {
                    enqueue(UiEvent{UiEvent::Type::Delta, "\r\n[aborted]\n", "", ""});
                }
            }
            if (event.type == AgentEvent::Type::AgentEnd)
            {
                enqueue(UiEvent{UiEvent::Type::RunEnd, "", "", ""});
            }
        });
}

Repl::~Repl()
{
    session_.abort();
    PosixShell::kill_all_children();
    if (worker_.joinable()) worker_.join();
    if (event_pipe_[0] >= 0) close(event_pipe_[0]);
    if (event_pipe_[1] >= 0) close(event_pipe_[1]);
}

int Repl::run()
{
    print_banner();
    enter_raw_mode();

    while (!quit_requested_)
    {
        // 流式中不重绘提示符：否则 ^[[2K 擦行会抹掉刚输出的流式文本
        if (!streaming_)
        {
            draw_prompt();
        }

        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {event_pipe_[0], POLLIN, 0};
        // 定期唤醒以刷新空闲提示符中的本进程内存快照。
        const int ready = poll(fds, 2, 1000);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0)
        {
            if (!streaming_) draw_prompt();
            continue;
        }
        if (fds[1].revents & POLLIN)
        {
            char buffer[64];
            while (read(event_pipe_[0], buffer, sizeof(buffer)) > 0)
            {
            }
            drain_events();
            continue;
        }
        if (fds[0].revents & POLLIN)
        {
            handle_input();
        }
    }

    session_.abort();
    PosixShell::kill_all_children();
    // 先恢复终端再 join：worker 可能还在等长任务工具收尾（尽管工具已响应 abort）。
    // 终端回到 cooked 模式后，此后的 Ctrl+C 会以 SIGINT 直接终止进程，
    // 用户不会被卡在 raw mode（raw mode 下 ISIG 关闭，Ctrl+C 是死键）。
    restore_raw_mode();
    if (worker_.joinable() && !run_finished_.load())
    {
        std::cout << "(等待运行中的任务结束，Ctrl+C 可立即退出)" << std::endl;
    }
    if (worker_.joinable()) worker_.join();
    return 0;
}

void Repl::print_banner() { std::cout << kBanner << std::endl; }

ProcessMemoryUsage Repl::sample_memory()
{
    const ProcessMemoryUsage current = sample_process_memory();
    if (current.hasResident &&
        (!peak_memory_.hasResident || current.residentBytes > peak_memory_.residentBytes))
    {
        peak_memory_.residentBytes = current.residentBytes;
        peak_memory_.hasResident = true;
    }
    return current;
}

void Repl::print_status()
{
    std::cout << status_text(session_, sample_memory(), peak_memory_) << std::endl;
}

void Repl::enter_raw_mode()
{
    tcgetattr(STDIN_FILENO, &original_termios_);
    struct termios raw = original_termios_;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    // 启用 CSI-u/kitty 键盘协议（flags=1：消歧义），使 Shift+Enter 等修饰键可区分。
    // 不支持的终端忽略该序列，继续发送传统字节流（Shift+Enter 不可区分，
    // 请使用 Ctrl+J 或 Option+Enter 换行）。
    std::cout << "\x1b[>1u" << std::flush;
}

void Repl::restore_raw_mode()
{
    // 先弹栈退出键盘协议，避免终端停留在 CSI-u 模式下影响后续 shell 输入
    std::cout << "\x1b[<u" << std::flush;
    // 恢复进入 raw mode 前的完整终端设置（含 IXON 等自定义项），而不是重建近似值。
    // 注意：不能用 TCSAFLUSH/TCSADRAIN——macOS 上它们会等待 pty 输出队列完全排空，
    // 主端无读者时（如脚本驱动）会永久阻塞在 ioctl；退出路径用 TCSANOW 立即生效。
    tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
    std::cout << "\r\n";
}

// ---------- 渲染 ----------

void Repl::draw_prompt()
{
    const ProcessMemoryUsage memory = sample_memory();

    // 命令菜单（仅空闲且缓冲区以 / 开头时渲染）
    const bool menu = command_menu_active();
    std::vector<CommandEntry> menu_entries;
    if (menu)
    {
        menu_entries = matching_commands();
    }
    const size_t menu_shown = std::min(menu_entries.size(), kMenuMaxLines);
    const bool menu_overflow = menu_entries.size() > kMenuMaxLines;
    const int menu_lines = static_cast<int>(menu_shown) + (menu_overflow ? 1 : 0);

    const int total_lines = 1 + static_cast<int>(count_newlines(line_buffer_)) + menu_lines;

    // 1) 回到提示行行首，逐行清除上一次渲染的内容（多行输入 + 菜单行）
    if (rendered_cursor_row_ > 0) std::cout << "\033[" << rendered_cursor_row_ << "A";
    std::cout << "\r";
    for (int i = 0; i < rendered_lines_; ++i)
    {
        std::cout << "\033[2K";
        if (i + 1 < rendered_lines_) std::cout << "\033[1B";
    }
    // 2) 回到提示行行首重新渲染
    std::cout << "\r";
    if (rendered_lines_ > 1) std::cout << "\033[" << (rendered_lines_ - 1) << "A";
    std::cout << "\r";

    const std::string prompt_prefix = "[mem " + format_process_memory(memory) + "] pi> ";
    std::cout << prompt_prefix << line_buffer_;

    // 3) 渲染命令菜单（输入区之下）
    if (menu_lines > 0)
    {
        size_t max_name = 0;
        for (size_t i = 0; i < menu_shown; ++i)
        {
            max_name = std::max(max_name, menu_entries[i].name.size());
        }
        for (size_t i = 0; i < menu_shown; ++i)
        {
            std::string line = "  /" + menu_entries[i].name;
            line.append(max_name - menu_entries[i].name.size() + 2, ' ');
            line += menu_entries[i].description;
            std::cout << "\r\n\033[2K" << line;
        }
        if (menu_overflow)
        {
            std::cout << "\r\n\033[2K  ... (+" << (menu_entries.size() - kMenuMaxLines)
                      << " more)";
        }
    }

    // 4) 光标定位：按「行 + 显示宽度」回到光标位置（多行输入与全角字符感知）。
    //    首行还要加上提示符前缀的宽度（\r 回到的是该行第 0 列）。
    const size_t line_start = [&]
    {
        if (cursor_ == 0) return size_t{0};
        const size_t pos = line_buffer_.rfind('\n', cursor_ - 1);
        return pos == std::string::npos ? size_t{0} : pos + 1;
    }();
    const size_t col = display_width(line_buffer_.substr(line_start, cursor_ - line_start));
    const size_t row = count_newlines(line_buffer_.substr(0, cursor_));
    const int lines_above = total_lines - 1 - static_cast<int>(row);
    if (lines_above > 0) std::cout << "\033[" << lines_above << "A";
    std::cout << "\r";
    const size_t target_col = col + (row == 0 ? display_width(prompt_prefix) : 0);
    if (target_col > 0) std::cout << "\033[" << target_col << "C";

    rendered_lines_ = total_lines;
    rendered_cursor_row_ = static_cast<int>(row);
    std::cout.flush();
}

// ---------- 输入 ----------

void Repl::handle_input()
{
    const KeyEvent key = decode_key_event();
    if (key.type == KeyEvent::Type::None) return;

    if (streaming_)
    {
        switch (key.type)
        {
            case KeyEvent::Type::Esc:
                request_abort();
                break;
            case KeyEvent::Type::Enter:  // steer：把已输入的行入队
                if (!line_buffer_.empty())
                {
                    session_.steer(Message::user(line_buffer_));
                    line_buffer_.clear();
                    cursor_ = 0;
                    rendered_lines_ = 1;
                    rendered_cursor_row_ = 0;
                }
                break;
            case KeyEvent::Type::CtrlC:
            {
                std::cout << "\r\n(退出确认：再按 Ctrl+C 退出，其他键取消)" << std::endl;
                if (stdin_has_data(1000))
                {
                    const KeyEvent next_key = decode_key_event();
                    if (next_key.type == KeyEvent::Type::CtrlC)
                    {
                        quit_requested_ = true;
                        session_.abort();
                    }
                    else
                    {
                        std::cout << "(已取消)" << std::endl;
                    }
                }
                else
                {
                    std::cout << "(已取消)" << std::endl;
                }
                break;
            }
            default:
                break;
        }
        return;
    }

    switch (key.type)
    {
        case KeyEvent::Type::CtrlC:
            quit_requested_ = true;
            break;
        case KeyEvent::Type::Left:
            if (cursor_ > 0) --cursor_;
            break;
        case KeyEvent::Type::Right:
            if (cursor_ < line_buffer_.size()) ++cursor_;
            break;
        case KeyEvent::Type::Home:
            cursor_ = 0;
            break;
        case KeyEvent::Type::End:
            cursor_ = line_buffer_.size();
            break;
        case KeyEvent::Type::Up:  // 上一条历史
            if (!history_.empty())
            {
                if (history_index_ == -1)
                {
                    saved_line_ = line_buffer_;
                }
                if (history_index_ < static_cast<int>(history_.size()) - 1)
                {
                    ++history_index_;
                    line_buffer_ = history_[history_.size() - 1 - history_index_];
                    cursor_ = line_buffer_.size();
                }
            }
            break;
        case KeyEvent::Type::Down:  // 下一条历史
            if (history_index_ >= 0)
            {
                --history_index_;
                if (history_index_ >= 0)
                {
                    line_buffer_ = history_[history_.size() - 1 - history_index_];
                }
                else
                {
                    line_buffer_ = saved_line_;
                }
                cursor_ = line_buffer_.size();
            }
            break;
        case KeyEvent::Type::Enter:
            submit_line();
            break;
        case KeyEvent::Type::ShiftEnter:  // 换行（Shift+Enter / Ctrl+J / Option+Enter）
            insert_at_cursor("\n");
            break;
        case KeyEvent::Type::Backspace:
            erase_char_before_cursor();
            break;
        case KeyEvent::Type::Tab:
            complete_command();
            break;
        case KeyEvent::Type::CtrlL:  // 清屏重绘
            std::cout << "\033[2J\033[H";
            rendered_lines_ = 1;
            rendered_cursor_row_ = 0;
            print_banner();
            break;
        case KeyEvent::Type::CtrlP:
            cycle_model();
            break;
        case KeyEvent::Type::Char:
        {
            const bool was_menu = command_menu_active();
            insert_at_cursor(key.text);
            // 首次键入 / 时刷新用户模板命令
            if (command_menu_active() && !was_menu) refresh_template_commands();
            break;
        }
        case KeyEvent::Type::Esc:
        default:
            break;
    }
}

void Repl::insert_at_cursor(const std::string& text)
{
    line_buffer_.insert(line_buffer_.begin() + static_cast<ptrdiff_t>(cursor_), text.begin(),
                        text.end());
    cursor_ += text.size();
}

void Repl::erase_char_before_cursor()
{
    if (cursor_ == 0) return;
    // 按 UTF-8 序列删除，避免删掉半个中文字符
    size_t start = cursor_ - 1;
    while (start > 0 && (static_cast<unsigned char>(line_buffer_[start]) & 0xC0) == 0x80)
    {
        --start;
    }
    line_buffer_.erase(start, cursor_ - start);
    cursor_ = start;
}

void Repl::submit_line()
{
    // 输出位置定位到输入区（多行）与命令菜单之下，且保持已输入的文本可见
    const size_t total_newlines = count_newlines(line_buffer_);
    const size_t cursor_row = count_newlines(line_buffer_.substr(0, cursor_));
    const int rows_below_input = static_cast<int>(total_newlines - cursor_row);
    for (int i = 0; i < rows_below_input; ++i) std::cout << "\r\n";
    const int menu_lines = command_menu_active()
                               ? std::min<int>(static_cast<int>(matching_commands().size()),
                                               static_cast<int>(kMenuMaxLines))
                               : 0;
    for (int i = 0; i < menu_lines; ++i) std::cout << "\r\n\033[2K";
    if (menu_lines > 0) std::cout << "\033[" << menu_lines << "A";
    std::cout << "\r\n";

    std::string line = line_buffer_;
    line_buffer_.clear();
    cursor_ = 0;
    history_index_ = -1;
    rendered_lines_ = 1;
    rendered_cursor_row_ = 0;

    if (!line.empty())
    {
        // 记入历史（跳过连续重复）
        if (history_.empty() || history_.back() != line)
        {
            history_.push_back(line);
        }
        // 去掉前导空白：` /clear` 应视为命令而非 prompt
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos)
        {
            line = line.substr(first);
        }
        if (line.front() == '/')
        {
            handle_command(line);
        }
        else if (!line.empty())
        {
            start_run(line);
        }
    }
}

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
        while (k < prefix.size() && k < matches[i].name.size() &&
               prefix[k] == matches[i].name[k])
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
            std::cout << "可用模型：deepseek-v4-flash, deepseek-v4-pro" << std::endl;
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
                    std::cout << "thinking → " << to_string(clamped) << "（"
                              << to_string(*level) << " 当前模型不支持，已自动回退）" << std::endl;
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
            std::cout << "已恢复最近会话（" << session_.messages().size() << " 条消息）" << std::endl;
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
                std::cout << "已恢复会话（" << session_.messages().size() << " 条消息）" << std::endl;
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
        session_.set_tools(make_coding_tools(cwd_, session_.api_key()));
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
    const std::string current = session_.model().id;
    const auto old_thinking = session_.thinking_level();
    if (current == "deepseek-v4-flash")
    {
        session_.set_model("deepseek-v4-pro");
    }
    else
    {
        session_.set_model("deepseek-v4-flash");
    }
    std::cout << "\r\nmodel → " << session_.model().id << std::endl;
    // set_model 内部已保证 thinking 合法；若发生了回退则明确提示（与 /model 命令一致）
    if (session_.thinking_level() != old_thinking)
    {
        std::cout << "（thinking 已自动回退：" << to_string(old_thinking) << " → "
                  << to_string(session_.thinking_level()) << "）" << std::endl;
    }
}

void Repl::request_abort()
{
    std::cout << "\r\n(aborting...)" << std::endl;
    session_.abort();
}

void Repl::start_run(const std::string& line)
{
    streaming_ = true;
    run_finished_.store(false);

    worker_ = std::thread(
        [this, line]
        {
            try
            {
                session_.prompt(line);
            }
            catch (const std::exception& e)
            {
                enqueue(UiEvent{UiEvent::Type::Delta, "[error] " + std::string(e.what()) + "\n",
                                ""});
                enqueue(UiEvent{UiEvent::Type::RunEnd, "", "", ""});
            }
            run_finished_.store(true);
        });
}

void Repl::enqueue(UiEvent event)
{
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.push_back(std::move(event));
    }
    char byte = 1;
    ssize_t ignored = write(event_pipe_[1], &byte, 1);
    (void)ignored;
}

void Repl::drain_events()
{
    std::vector<UiEvent> events;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        events.assign(event_queue_.begin(), event_queue_.end());
        event_queue_.clear();
    }

    for (const auto& event : events)
    {
        switch (event.type)
        {
            case UiEvent::Type::Delta:
                std::cout << event.text << std::flush;
                break;
            case UiEvent::Type::ToolStart:
                if (event.toolName == "bash" && !event.detail.empty())
                {
                    std::cout << "\r\n\033[2K[tool:bash] " << event.detail << std::endl;
                }
                else if (event.toolName == "edit" && !event.detail.empty())
                {
                    std::cout << "\r\n\033[2K[tool:edit] " << event.detail << std::endl;
                }
                else
                {
                    std::cout << "\r\n\033[2K[tool] " << event.toolName << " ..." << std::endl;
                }
                break;
            case UiEvent::Type::ToolEnd:
            {
                if (event.toolName == "edit" && !event.detail.empty())
                {
                    std::cout << event.detail << std::endl;
                }
                else if (event.toolName == "bash" && !event.detail.empty())
                {
                    // 截断输出：前 600 字符或前 15 行，以较短行数为准
                    constexpr size_t kMaxChars = 600;
                    constexpr int kMaxLines = 15;
                    std::string truncated = event.detail;
                    if (truncated.size() > kMaxChars)
                    {
                        truncated.resize(kMaxChars);
                    }
                    int line_count = 0;
                    for (char c : truncated)
                    {
                        if (c == '\n') ++line_count;
                    }
                    if (line_count > kMaxLines)
                    {
                        size_t pos = 0;
                        for (int i = 0; i < kMaxLines && pos < truncated.size(); ++pos)
                        {
                            if (truncated[pos] == '\n') ++i;
                        }
                        truncated.resize(pos);
                    }
                    std::cout << truncated;
                    if (truncated.size() < event.detail.size())
                        std::cout << "\n(truncated, total " << event.detail.size()
                                  << " chars)";
                    std::cout << std::endl;
                }
                std::cout << "\r\033[2K[tool] " << event.toolName << " done" << std::endl;
                break;
            }
            case UiEvent::Type::RunEnd:
                if (worker_.joinable()) worker_.join();
                streaming_ = false;
                if (session_.last_turn_usage().totalTokens > 0)
                {
                    std::cout << "\r\n" << format_usage_line(session_.last_turn_usage())
                              << std::endl;
                }
                print_status();
                break;
        }
    }
}

}  // namespace pi

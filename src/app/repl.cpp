#include "pi/app/repl.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "pi/ai/model_registry.h"
#include "pi/app/commands.h"
#include "pi/app/images.h"
#include "pi/harness/prompt_templates.h"
#include "pi/harness/skills.h"
#include "pi/util/scope_exit.h"
#include "terminal_input.h"

namespace pi
{
using terminal::count_newlines;
using terminal::decode_key_event;
using terminal::display_width;
using terminal::KeyEvent;
using terminal::stdin_has_data;

namespace
{

constexpr const char* kBanner =
    "pi-cpp — C++ terminal agent (pi 移植版)\n"
    "输入 / 唤起命令菜单（Tab 补全）。Enter=提交  Shift+Enter/Ctrl+J/Option+Enter=换行\n"
    "流式中：Enter=steer Esc=abort Ctrl+C=退出（确认）Ctrl+P=切模型 Ctrl+L=重绘\n";

/** 命令菜单最多显示的条目数。 */
constexpr size_t kMenuMaxLines = 8;

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
    ScopeExit restore_terminal([this]() noexcept { restore_raw_mode(); });

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
            try
            {
                handle_input();
            }
            catch (const std::exception& error)
            {
                std::cout << "\r\n[error] " << error.what() << std::endl;
            }
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
    if (tcgetattr(STDIN_FILENO, &original_termios_) != 0)
        throw std::runtime_error("Cannot read terminal settings");
    struct termios raw = original_termios_;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        throw std::runtime_error("Cannot enter terminal raw mode");
    raw_mode_ = true;
    // 启用 CSI-u/kitty 键盘协议（flags=1：消歧义），使 Shift+Enter 等修饰键可区分。
    // 不支持的终端忽略该序列，继续发送传统字节流（Shift+Enter 不可区分，
    // 请使用 Ctrl+J 或 Option+Enter 换行）。
    std::cout << "\x1b[>1u" << std::flush;
}

void Repl::restore_raw_mode()
{
    if (!raw_mode_) return;
    raw_mode_ = false;
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
            std::cout << "\r\n\033[2K  ... (+" << (menu_entries.size() - kMenuMaxLines) << " more)";
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
                enqueue(UiEvent{UiEvent::Type::Delta, "[error] " + std::string(e.what()) + "\n", "",
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
                        std::cout << "\n(truncated, total " << event.detail.size() << " chars)";
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
                    std::cout << "\r\n"
                              << format_usage_line(session_.last_turn_usage()) << std::endl;
                }
                print_status();
                break;
        }
    }
}

}  // namespace pi

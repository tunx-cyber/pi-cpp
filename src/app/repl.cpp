#include "pi/app/repl.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>

#include <iostream>
#include <sstream>
#include <vector>

#include "pi/ai/model_registry.h"
#include "pi/app/images.h"
#include "pi/harness/prompt_templates.h"
#include "pi/harness/skills.h"

namespace pi
{

namespace
{

constexpr const char* kBanner =
    "pi-cpp — C++ terminal agent (pi 移植版)\n"
    "输入 /help 查看命令。流式中：Enter=steer Esc=abort Ctrl+C=退出\n";

std::string status_text(const AgentSession& session, const ProcessMemoryUsage& memory,
                        const ProcessMemoryUsage& peak_memory)
{
    const ModelInfo model = session.model();
    std::string thinking = to_string(session.thinking_level());
    std::string out = "[model=" + model.id + "] [thinking=" + thinking + "]";
    out += " [turn=$" + std::to_string(session.total_cost()) + "]";
    out += " [all=$" + std::to_string(session.all_time_cost()) + "]";
    out += " [mem " + format_process_memory(memory);
    if (peak_memory.hasResident)
    {
        out += " peak_rss=" + format_memory_bytes(peak_memory.residentBytes);
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
                    enqueue(UiEvent{UiEvent::Type::Delta, stream_event.delta, "", "", ""});
                }
            }
            if (event.type == AgentEvent::Type::ToolExecutionStart)
            {
                // 提取工具特有信息用于展示
                std::string detail;
                if (event.toolName == "bash" && event.args.contains("command"))
                {
                    detail = event.args["command"].get<std::string>();
                }
                else if (event.toolName == "edit" && event.args.contains("path"))
                {
                    detail = event.args["path"].get<std::string>();
                }
                enqueue(UiEvent{UiEvent::Type::ToolStart, "", event.toolName, "", std::move(detail)});
            }
            if (event.type == AgentEvent::Type::ToolExecutionEnd)
            {
                // 收集工具输出的文本部分；edit 工具生成 diff 格式
                std::string detail;
                if (event.toolName == "edit")
                {
                    const std::string old_str =
                        event.args.contains("oldString") ? event.args["oldString"].get<std::string>()
                                                         : "";
                    const std::string new_str =
                        event.args.contains("newString") ? event.args["newString"].get<std::string>()
                                                         : "";
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
                enqueue(UiEvent{UiEvent::Type::ToolEnd, "", event.toolName, "", std::move(detail)});
            }
            if (event.type == AgentEvent::Type::MessageEnd && event.message.role == Role::Assistant)
            {
                if (event.message.stopReason == StopReason::Error)
                {
                    const std::string message = event.message.errorMessage.empty()
                                                    ? "AI request failed"
                                                    : event.message.errorMessage;
                    enqueue(UiEvent{UiEvent::Type::Delta, "\r\n[error] " + message + "\n", "", "",
                                    ""});
                }
                else if (event.message.stopReason == StopReason::Aborted)
                {
                    enqueue(UiEvent{UiEvent::Type::Delta, "\r\n[aborted]\n", "", "", ""});
                }
            }
            if (event.type == AgentEvent::Type::AgentEnd)
            {
                enqueue(UiEvent{UiEvent::Type::RunEnd, "", "", "", ""});
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
    if (current.hasVirtual &&
        (!peak_memory_.hasVirtual || current.virtualBytes > peak_memory_.virtualBytes))
    {
        peak_memory_.virtualBytes = current.virtualBytes;
        peak_memory_.hasVirtual = true;
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
}

void Repl::restore_raw_mode()
{
    // 恢复进入 raw mode 前的完整终端设置（含 IXON 等自定义项），而不是重建近似值
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios_);
    std::cout << "\r\n";
}

void Repl::draw_prompt()
{
    const ProcessMemoryUsage memory = sample_memory();
    std::cout << "\r\033[2K[mem " << format_process_memory(memory) << "] pi> " << line_buffer_;
    if (cursor_ < line_buffer_.size())
    {
        std::cout << "\033[" << (line_buffer_.size() - cursor_) << "D";
    }
    std::cout.flush();
}

void Repl::handle_input()
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;

    if (streaming_)
    {
        switch (c)
        {
            case 27:  // Esc → abort
                request_abort();
                break;
            case '\r':
            case '\n':
            {  // Enter → steer
                if (!line_buffer_.empty())
                {
                    session_.steer(Message::user(line_buffer_));
                    line_buffer_.clear();
                    cursor_ = 0;
                }
                break;
            }
            case 3:  // Ctrl+C → 确认退出（流式中始终生效，不要求输入行已清空）
                {
                    std::cout << "\r\n(退出确认：再按 Ctrl+C 退出，其他键取消)" << std::endl;
                    char next;
                    if (read_byte_with_timeout(next, 1000) && next == 3)
                    {
                        quit_requested_ = true;
                        session_.abort();
                    }
                    else
                    {
                        std::cout << "(已取消)" << std::endl;
                    }
                }
                break;
            default:
                break;
        }
        return;
    }

    switch (c)
    {
        case 3:  // Ctrl+C → quit
            quit_requested_ = true;
            break;
        case 27:
        {  // Esc 序列（后续字节带超时读取，裸按 Esc 不再挂起 UI）
            char seq[2];
            if (read_byte_with_timeout(seq[0], 50) && read_byte_with_timeout(seq[1], 50))
            {
                if (seq[0] == '[' && seq[1] == 'D' && cursor_ > 0)
                {  // 左方向键
                    --cursor_;
                }
                else if (seq[0] == '[' && seq[1] == 'C' && cursor_ < line_buffer_.size())
                {  // 右方向键
                    ++cursor_;
                }
                else if (seq[0] == '[' && seq[1] == 'A')
                {  // 上方向键：上一条历史
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
                }
                else if (seq[0] == '[' && seq[1] == 'B')
                {  // 下方向键：下一条历史
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
                }
            }
            break;
        }
        case '\r':
        case '\n':
        {
            std::cout << "\r\n";
            std::string line = line_buffer_;
            line_buffer_.clear();
            cursor_ = 0;
            history_index_ = -1;
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
            break;
        }
        case 127:
        case 8:
        {  // Backspace（按 UTF-8 序列删除，避免删掉半个中文字符）
            if (cursor_ > 0)
            {
                size_t start = cursor_ - 1;
                while (start > 0 &&
                       (static_cast<unsigned char>(line_buffer_[start]) & 0xC0) == 0x80)
                {
                    --start;
                }
                line_buffer_.erase(start, cursor_ - start);
                cursor_ = start;
            }
            break;
        }
        case 12:  // Ctrl+L → 重绘
            std::cout << "\033[2J\033[H";
            print_banner();
            break;
        case 16:  // Ctrl+P → 切换模型
            cycle_model();
            break;
        default:
            // 用 unsigned char 比较：UTF-8 中文字节是负数，直接丢弃导致中文无法输入
            if (static_cast<unsigned char>(c) >= 32)
            {
                line_buffer_.insert(line_buffer_.begin() + static_cast<ptrdiff_t>(cursor_), c);
                ++cursor_;
            }
            break;
    }
}

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
        session_.resume();
        std::cout << "已恢复最近会话（" << session_.messages().size() << " 条消息）" << std::endl;
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
        std::cout << "会话（" << sessions.size() << "）：" << std::endl;
        for (const auto& s : sessions)
        {
            std::cout << "  " << s.createdAt << " " << s.id << " " << s.path << std::endl;
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
            std::cout << " peak_rss=" << format_memory_bytes(peak_memory_.residentBytes);
        }
        if (peak_memory_.hasVirtual)
        {
            std::cout << " peak_vms=" << format_memory_bytes(peak_memory_.virtualBytes);
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
        session_.set_tools(make_coding_tools(cwd_));
        std::cout << "已启用编码工具：read/bash/edit/write/grep/find/ls" << std::endl;
        return;
    }

    // 用户模板：~/.pi-cpp/templates/<name>.md → /<name> args
    const auto templates =
        load_prompt_templates(fs_, {Settings::expand_home("~/.pi-cpp/templates")});
    for (const auto& template_ : templates.promptTemplates)
    {
        if (template_.name == name)
        {
            const auto args =
                parse_command_args(space == std::string::npos ? "" : line.substr(space + 1));
            start_run(format_prompt_template_invocation(template_, args));
            return;
        }
    }
    std::cout << "未知命令 /" << name << "（/help 查看）" << std::endl;
}

std::string Repl::help_text()
{
    return "命令：\n"
           "  /help        显示帮助\n"
           "  /model [id]  查看或切换模型（deepseek-v4-flash / deepseek-v4-pro）\n"
           "  /thinking [l]查看或切换 thinking（当前模型支持："
           + join_thinking_levels(get_supported_thinking_levels(session_.model())) + "）\n"
           "  /compact     手动压缩会话\n"
           "  /clear       清空会话\n"
           "  /new         新会话\n"
           "  /resume      恢复最近会话\n"
           "  /sessions    列出会话\n"
           "  /image <path> 图片输入\n"
           "  /tools       启用编码工具（read/bash/edit/write/grep/find/ls）\n"
           "  /skills      列出 skills\n"
           "  /memory      显示 picpp 进程当前及峰值内存\n"
           "  /quit        退出\n"
           "用户模板：~/.pi-cpp/templates/<name>.md → /<name> 参数\n"
           "流式中：Enter=steer Esc=abort Ctrl+C=退出（确认）Ctrl+P=切模型 Ctrl+L=重绘\n";
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
            case UiEvent::Type::Status:
                std::cout << event.statusLine << std::endl;
                break;
        }
    }
}

}  // namespace pi

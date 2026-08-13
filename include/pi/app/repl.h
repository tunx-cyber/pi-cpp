#pragma once

#include <termios.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pi/app/agent_session.h"
#include "pi/app/memory_monitor.h"
#include "pi/harness/env.h"

namespace pi
{

/** slash 命令条目（内建命令与用户模板共用，供命令菜单与 /help 使用）。 */
struct CommandEntry
{
    std::string name;
    std::string description;
};

/**
 * 终端 REPL：poll(stdin, event_pipe) 循环 + 行编辑 + 事件泵 + 状态栏。
 *
 * 输入（空闲）：
 * - 缓冲区首个字符为 `/` 时唤起命令菜单（实时过滤 + Tab 补全），Enter 执行；
 * - Enter 提交（支持多行输入），Shift+Enter / Ctrl+J / Option+Enter 换行。
 *   Shift+Enter 依赖终端的 CSI-u/kitty 键盘协议（iTerm2/kitty/WezTerm 支持，
 *   macOS Terminal.app 不支持，请用 Ctrl+J 或 Option+Enter）；
 * - 上/下方向键历史，左/右移动光标，Backspace 按 UTF-8 字符删除。
 * 流式中：Enter=steer、Esc=abort、Ctrl+C=退出（确认）、Ctrl+P=切模型、Ctrl+L=重绘。
 */
class Repl
{
   public:
    Repl(AgentSession& session, std::string cwd);
    ~Repl();

    int run();
    void quit() { quit_requested_ = true; }
    void request_abort();
    void cycle_model();

   private:
    struct UiEvent
    {
        enum class Type
        {
            Delta,
            RunEnd,
            ToolStart,
            ToolEnd,
        };
        Type type = Type::Delta;
        std::string text;
        std::string toolName;
        std::string detail;  // ToolStart=命令参数, ToolEnd=截断后的输出
    };

    // ---------- 输入 ----------

    void enter_raw_mode();
    void restore_raw_mode();
    void handle_input();
    void submit_line();
    void insert_at_cursor(const std::string& text);
    void erase_char_before_cursor();
    void handle_command(const std::string& line);
    void start_run(const std::string& line);

    // ---------- 渲染 ----------

    void draw_prompt();
    void drain_events();
    void enqueue(UiEvent event);
    void print_banner();
    void print_status();
    ProcessMemoryUsage sample_memory();
    std::string help_text();

    // ---------- 命令菜单（首个字符为 / 时唤起） ----------

    bool command_menu_active() const;
    std::vector<CommandEntry> matching_commands() const;
    void refresh_template_commands();
    void complete_command();

    AgentSession& session_;
    std::string cwd_;
    PosixFileSystem fs_;

    // UI 状态
    std::string line_buffer_;
    size_t cursor_ = 0;
    bool streaming_ = false;
    bool quit_requested_ = false;
    struct termios original_termios_;  // 进入 raw mode 前的终端设置，退出时完整恢复
    int rendered_lines_ = 1;  // draw_prompt 上次渲染的总行数（提示 + 多行输入 + 菜单）
    int rendered_cursor_row_ = 0;  // 上次渲染时光标所在行（0 基，用于回到提示行首）
    std::vector<CommandEntry> template_commands_;  // 用户模板命令（键入 / 时刷新）

    // 历史记录
    std::vector<std::string> history_;
    int history_index_ = -1;
    std::string saved_line_;  // 导航前暂存当前输入

    // 事件通道：worker → UI
    std::mutex event_mutex_;
    std::deque<UiEvent> event_queue_;
    int event_pipe_[2] = {-1, -1};
    std::thread worker_;
    std::atomic<bool> run_finished_{true};  // worker 收尾标志（退出路径用于提示用户）

    // 内存峰值监控（状态栏 /memory 命令共用）
    ProcessMemoryUsage peak_memory_;
};

}  // namespace pi

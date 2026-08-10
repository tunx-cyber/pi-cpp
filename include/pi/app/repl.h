#pragma once

#include <condition_variable>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pi/app/agent_session.h"
#include "pi/app/commands.h"
#include "pi/app/memory_monitor.h"
#include "pi/harness/env.h"

namespace pi
{

/**
 * 终端 REPL：poll(stdin, event_pipe) 循环 + 行编辑 + 事件泵 + 状态栏。
 * 流式时 Enter=steer、Esc=abort、Ctrl+C=退出（确认）、Ctrl+P=切模型、Ctrl+L=重绘。
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
            Status
        };
        Type type = Type::Delta;
        std::string text;
        std::string toolName;
        std::string statusLine;
        std::string detail;  // ToolStart=命令参数, ToolEnd=截断后的输出
    };

    void enter_raw_mode();
    void restore_raw_mode();
    void draw_prompt();
    void handle_input();
    void handle_command(const std::string& line);
    void start_run(const std::string& line);
    void drain_events();
    void enqueue(UiEvent event);
    void print_banner();
    void print_status();
    ProcessMemoryUsage sample_memory();
    std::string help_text();

    AgentSession& session_;
    std::string cwd_;
    PosixFileSystem fs_;
    CommandRegistry registry_;

    // UI 状态
    std::string line_buffer_;
    size_t cursor_ = 0;
    bool streaming_ = false;
    bool quit_requested_ = false;
    bool run_active_ = false;

    // 历史记录
    std::vector<std::string> history_;
    int history_index_ = -1;
    std::string saved_line_;  // 导航前暂存当前输入

    // 事件通道：worker → UI
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    std::deque<UiEvent> event_queue_;
    int event_pipe_[2] = {-1, -1};
    std::thread worker_;

    // 渲染状态
    int64_t total_input_tokens_ = 0;
    int64_t total_output_tokens_ = 0;
    double session_cost_ = 0;
    ProcessMemoryUsage peak_memory_;
};

}  // namespace pi

#pragma once

#include <termios.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pi/app/agent_session.h"
#include "pi/app/memory_monitor.h"
#include "pi/app/theme.h"
#include "pi/harness/env.h"

namespace pi
{

namespace terminal
{
struct KeyEvent;  // 定义在 src/app/terminal_input.h（避免 include 层反向依赖 src）
}  // namespace terminal

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
 * - 缓冲区首个字符为 `/` 时唤起命令菜单：↑/↓ 选择、Tab 补全、Enter 执行（实时过滤）；
 * - `/model` 打开模型选择器：↑/↓ 选择、Enter 确认、Esc/Ctrl+C 取消；
 * - Enter 提交（支持多行输入），Shift+Enter / Ctrl+J / Option+Enter 换行。
 *   Shift+Enter 依赖终端的 CSI-u/kitty 键盘协议（iTerm2/kitty/WezTerm 支持，
 *   macOS Terminal.app 不支持，请用 Ctrl+J 或 Option+Enter）；
 * - 菜单未打开时上/下方向键为历史，左/右移动光标，Backspace 按 UTF-8 字符删除。
 * 流式中：Enter=steer、Esc=abort、Ctrl+C=退出（确认）、Ctrl+P=切模型、Ctrl+L=重绘。
 */
class Repl
{
   public:
    Repl(AgentSession& session, std::string cwd, Theme theme = {},
         bool show_history_on_start = false);
    ~Repl();

    int run();
    void quit() { quit_requested_ = true; }
    void request_abort();
    void cycle_model();

   private:
    /** 命令菜单/模型选择器最多显示的条目数（超出部分滚动）。 */
    static constexpr size_t kMenuMaxLines = 8;

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
        std::string detail;    // ToolStart=命令参数, ToolEnd=截断后的输出
        bool isError = false;  // ToolEnd：工具失败（打印 failed 而不是 done）
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
    /** 在 worker 线程执行手动压缩（不阻塞 UI；Esc 可取消）。 */
    void start_compact();

    // ---------- 渲染 ----------

    void draw_prompt();
    /** 清除上一次 draw_prompt 渲染的区域（提示行 + 多行输入 + 菜单/选择器），光标回到顶部行首。 */
    void clear_rendered_area();
    /** 在当前提示区打印一行文本（先清掉菜单/选择器，避免覆盖）。 */
    void print_above_prompt(const std::string& text);
    void drain_events();
    void enqueue(UiEvent event);
    void print_banner();
    void print_status();
    /** 把当前会话的完整对话回放到终端（/resume 与 --resume 启动时）。 */
    void print_conversation();
    ProcessMemoryUsage sample_memory();
    std::string help_text();

    // ---------- 命令菜单（首个字符为 / 时唤起） ----------

    bool command_menu_active() const;
    std::vector<CommandEntry> matching_commands() const;
    /** 渲染菜单行（含选中高亮与滚动窗口）。 */
    std::vector<std::string> command_menu_lines() const;
    void move_menu_selection(int delta);
    /** 把当前选中的菜单项写入输入缓冲区（不提交）。 */
    void accept_menu_selection();
    void refresh_template_commands();
    void complete_command();

    // ---------- 选择器（/model、/thinking 共用） ----------

    enum class PickerKind
    {
        Model,
        Thinking,
    };

    std::vector<std::string> picker_lines() const;
    void handle_picker_key(const terminal::KeyEvent& key);
    void open_model_picker();
    void open_thinking_picker();
    /** 打开通用列表选择器（↑/↓ 选择、Enter 确认、Esc 取消）。 */
    void open_picker(PickerKind kind, std::vector<std::string> items, std::string title);
    void close_picker(const std::string& message);
    void apply_picker_selection();
    /** 切换模型；返回给用户看的一行文本。 */
    std::string apply_model(const std::string& model_id);
    /** 切换 thinking 级别（不做模型能力校验）；返回给用户看的一行文本。 */
    std::string apply_thinking(const std::string& level_name);

    // ---------- 模型列表（OpenAI 兼容 GET /models） ----------

    /** 拉取端点模型列表并缓存；force=true 强制重新请求。返回是否成功。 */
    bool load_remote_models(bool force);
    /** 可切换的模型 id：远端列表优先，请求失败/为空时回退注册表。 */
    std::vector<std::string> available_model_ids();

    AgentSession& session_;
    std::string cwd_;
    PosixFileSystem fs_;
    Theme theme_;
    bool show_history_on_start_ = false;

    // UI 状态
    std::string line_buffer_;
    size_t cursor_ = 0;
    bool streaming_ = false;
    bool prompt_run_ = false;  // 当前 worker 是 prompt 轮次（false = 压缩）
    bool raw_mode_ = false;
    bool quit_requested_ = false;
    struct termios original_termios_;  // 进入 raw mode 前的终端设置，退出时完整恢复
    int rendered_lines_ = 1;           // draw_prompt 上次渲染的总行数（提示 + 多行输入 + 菜单）
    int rendered_extra_lines_ = 0;     // 其中输入区下方（菜单/选择器）占用的行数
    int rendered_cursor_row_ = 0;      // 上次渲染时光标所在行（0 基，用于回到提示行首）
    size_t menu_index_ = 0;            // 命令菜单选中项（matching_commands 下标）
    std::vector<CommandEntry> template_commands_;  // 用户模板命令（键入 / 时刷新）

    // 选择器状态（/model、/thinking 无参数时打开）
    bool picker_active_ = false;
    PickerKind picker_kind_ = PickerKind::Model;
    std::vector<std::string> picker_items_;
    std::string picker_title_;
    size_t picker_index_ = 0;

    // 远端模型列表缓存（/model、Ctrl+P 共用；/model 每次强制刷新）
    std::vector<std::string> remote_model_ids_;
    bool remote_models_loaded_ = false;
    std::string remote_models_error_;

    // 历史记录
    std::vector<std::string> history_;
    int history_index_ = -1;
    std::string saved_line_;  // 导航前暂存当前输入

    // 事件通道：worker → UI
    std::mutex event_mutex_;
    std::deque<UiEvent> event_queue_;
    int event_pipe_[2] = {-1, -1};
    std::thread worker_;
    std::function<void()> unsubscribe_;     // 事件订阅句柄（析构时退订，避免悬空回调）
    std::atomic<bool> run_finished_{true};  // worker 收尾标志（退出路径用于提示用户）

    // 内存峰值监控（状态栏 /memory 命令共用）
    ProcessMemoryUsage peak_memory_;
};

}  // namespace pi

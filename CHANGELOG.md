# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式（无版本号，按日期记录）。

## 2026-08-13（二）REPL 交互：命令菜单与多行输入

### 新增

- **命令菜单**：输入 `/`（缓冲区首个字符）唤起实时过滤的菜单（内建命令 + 用户模板，最多 8 条），Tab 补全到最长公共前缀（唯一匹配补全并加空格）；内建命令表 `builtin_command_entries()` 与 /help 共用单一来源。
- **多行输入**：`Shift+Enter` / `Ctrl+J` / `Option+Enter` 插入换行（三种编码都映射为换行，覆盖支持与不支持 CSI-u 的终端）。
- **CSI-u/kitty 键盘协议**：进入 raw mode 发送 `CSI > 1 u` 启用（iTerm2/kitty/WezTerm），退出弹栈；按键统一经 `decode_key_event` 解码，传统字节流与 CSI-u 编码下 Ctrl+C/Esc/方向键等行为一致。
- **全角宽度感知的光标定位**：`display_width` 近似 wcwidth（中文/全角/emoji 计 2 列，无 locale 依赖），多行输入下光标与重绘对齐；Home/End 键支持。

### 修复

- **macOS 退出阻塞**：`restore_raw_mode` 从 TCSAFLUSH 改为 TCSANOW——macOS 上 TCSAFLUSH/TCSADRAIN 会等待 pty 输出队列排空，主端无读者时（脚本驱动场景）永久阻塞在 ioctl，表现为 Ctrl+C 后进程"假死"（pty 复现 10/10，修复后 0/10）。
- **LF 回车丢失**：部分终端在 raw mode 下因 ICRNL 将 Enter 送达为 `\n`，此前只处理 `\r` 导致 Enter 失效。
- **/compact 对空对话发起 LLM 调用**：`prepare_compaction` 无可摘要消息时（如新会话只有 model_change/thinking_level_change 条目）返回空 preparation，改为立即报 "Nothing to compact"；/compact 执行前提示「压缩中…」（摘要调用在 UI 线程同步执行）。
- 流式中 Ctrl+C 二次确认改为按解码后的事件匹配（CSI-u 终端下第二次 Ctrl+C 也能识别）。

### 重构

- 移除死代码：`CommandRegistry`/`CommandContext`（从未被使用）、Repl 的 `registry_`/`event_cv_`/未使用的 token 累计成员、`UiEvent::Status` 事件类型。
- 渲染状态重构：`rendered_lines_`/`rendered_cursor_row_` 跟踪提示块，多行输入与菜单统一重绘。

### 文档 / 测试

- TECHNICAL.md §5.3 重写（键盘协议、命令菜单、多行输入、TCSANOW 注意事项），§4.2 补 compaction 空摘要语义，维护清单补 REPL pty 冒烟项。
- 新增 compaction 回归测试（无可摘要消息 → nullopt；split-turn 切点检测）。全套 118 用例，tsan 0 警告。
- pty 冒烟（脚本驱动）：菜单/过滤/Tab 补全/Shift+Enter/Ctrl+J/中文/CSI-u 提交/空闲 Ctrl+C 退出，全部通过。

## 2026-08-13

### 修复

- **压缩摘要 prompt 重复注入**：`generate_summary_internal` 不再二次包裹 `<conversation>`，摘要请求中的对话内容只出现一次（此前 token 翻倍、影响摘要质量）。新增请求体级回归测试。
- **会话存储并发竞争**：`JsonlSessionStorage` 全部公开方法加 mutex；新增 `append_child` 单次加锁完成「生成 id → 挂到当前 leaf → 落盘」。修复 UI 线程（/model、/thinking、/tools）与 run 线程（消息持久化）并发追加时的数据竞争与孤儿分支。
- **uuidv7 线程安全**：内部状态（时间戳/序列号/随机引擎）串行化。
- **成本累计数据竞争**：`total_cost_`/`all_time_cost_`/`last_turn_usage_` 读写统一加锁（run 线程写、UI 线程读）。
- **会话尾行损坏自愈**：进程在追加中途被强杀留下的半行 JSON，`open()` 自动截断到最后一个完整行并写回；文件中部的损坏仍然拒绝打开。
- **openai_transport 注入配对**：`requiresAssistantAfterToolResult` 插入的桥接 assistant 消息同步推空 extras，保证 `assistantExtras` 与请求体序号对应不变量，消除潜在错位。
- **重复 Authorization 头**：移除手动注入，改由 openai-cpp 客户端 `use_bearer_auth` 统一注入。
- **subagent maxTurns 误报**：恰好跑满 maxTurns 但最后一轮给出干净最终回答时不再误报「max turns exceeded」，仅在工作被截断时报错。

### 强化

- **abort 全链路响应**：read/grep/find 工具在遍历循环中检查 abort（此前只有 bash 响应），Ctrl+C/Esc 后工具线程立即收尾并返回部分结果。
- **Ctrl+C 退出路径**：REPL 退出先恢复终端再 join worker——cooked 模式下 Ctrl+C 以 SIGINT 直接终止进程，不再卡在 raw mode（raw mode 下 ISIG 关闭，Ctrl+C 是死键）；流式中 Ctrl+C 确认不再要求输入行清空。
- **管道模式子进程清理**：退出前 kill 全部子进程组，避免 bash 工具遗留孤儿进程。
- **坏 chunk 诊断**：无法解析的 SSE chunk 计数并入「无 finish_reason」的最终错误信息。
- **REPL termios 恢复**：保存进入 raw mode 前的完整终端设置，退出时原样恢复（不再重建近似值）。
- **REPL Esc 序列超时读取**：裸按 Esc 不再挂起 UI。
- **PosixShell 环境变量支持**：`ExecOptions::env` 实现（fork 前构造 envp，经 `execle` 传入，避免 fork 后 malloc）。
- **skills 加载排除 build 目录**：与文档声明一致。

### 性能

- **parse_streaming_json 回退 O(n²) → 前缀扫描**：`longest_valid_json_prefix` 单次前向扫描收集候选末尾，避免大段未闭合字符串（write 工具的流式参数）逐字符重解析。

### 重构

- `subagent_tool` 误导命名 `error_result` → `text_result`。
- 移除 `compact` 的 `force` 死参数、`make_loop_config` 的 `skip` 死参数、REPL `run_active_` 死状态。
- `agent_session` 未知模型兜底构造去重为 `make_custom_model`。
- `handle_run_failure` 读取 `model_` 前加锁快照。
- 清理多处 unused include。

### 文档

- `docs/TECHNICAL.md`：修正用例数漂移（97/105 → 116）、tsan 措辞；同步全部行为变更；**新增 §12 维护指南**（必须保持的不变量、改动点速查表、提交前检查清单）。
- `README.md`：用例数、tsan 复用依赖技巧、bash 无人工确认的安全提示。
- 新增本文件（CHANGELOG.md）。

### 测试

- 新增：compaction 请求体级回归（2）、会话并发追加（1）、尾行截断自愈（1）、subagent maxTurns 干净收尾（1）、json_util（6）。
- 全套 **116 用例**通过；`-fsanitize=thread` 构建下 0 警告。

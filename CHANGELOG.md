# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式（无版本号，按日期记录）。

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

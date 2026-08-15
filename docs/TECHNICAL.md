# pi-cpp 技术文档

pi（TypeScript 终端 Agent）的 C++17 移植版。本文档描述代码实现的技术细节：架构、各模块设计、并发模型、wire 协议、会话格式、与 pi 的映射关系及测试策略。

## 1. 总体架构

分层严格镜像 pi，每层只依赖下一层：

```
app(REPL)  →  harness  →  agent  →  ai
```

```
pi-cpp/
├── include/pi/
│   ├── ai/       类型、事件、模型表、计费、SSE 解析、TransportAdapter 接口、openai-cpp 传输
│   ├── agent/    Agent 状态机、agent 循环、双队列、schema 校验、子 agent 工具
│   ├── harness/  会话(JSONL)、压缩、skills、模板、env(POSIX)、AgentHarness
│   └── app/      settings、图片、slash 命令、REPL、AgentSession
├── src/          与 include 一一对应的实现
├── apps/repl/main.cpp  薄入口（交互 REPL / 管道一次性对话）
└── tests/        googletest 单测 + test_utils（fake SSE server、脚本化 transport）
```

核心设计约束：

1. **`TransportAdapter` 是唯一知道 HTTP/SSE 协议的层**。上层全是接口与数据；用户自己的协议库在此对接，内置 openai-cpp 实现为默认适配器。
2. **事件是普通 struct + `std::function` sink**。`stream_chat` 同步阻塞地驱动事件流，上层通过 sink 消费。
3. **`stream_chat` 契约**（镜像 pi 的 StreamFunction）：不得抛异常；请求/模型/运行时失败一律以 `kError` 事件终止（`stopReason=error/aborted` + `errorMessage`）；正常终止为 `kDone`（`stopReason=stop/length/toolUse`）。
4. **阻塞为主，线程只出现在 pi 有并发的地方**：工具执行（每调用一个线程，上限 8）、子 agent（父工具线程内联）、UI 事件泵。

## 2. ai 层

### 2.1 类型系统（types.h / events.h）

- `Json = nlohmann::json`——单点别名，替换 JSON 库只改一处。
- `ContentBlock`：text / thinking（含 `thinkingSignature`）/ image（base64 + mime）/ toolCall（id/name/arguments/thoughtSignature）。
- `Message`：role + content 块 + assistant 字段（api/provider/model/responseId/usage/stopReason/errorMessage）+ toolResult 字段（toolCallId/toolName/isError/details）。
- `Usage`/`Cost`：input/output/cacheRead/cacheWrite/totalTokens + 分项人民币计价。
- `StreamEvent`：11 种类型（Start/TextStart/TextDelta/TextEnd/ThinkingStart/ThinkingDelta/ThinkingEnd/ToolCallStart/ToolCallDelta/ToolCallEnd/Done/Error），每个事件携带 partial message（镜像 `AssistantMessageEvent`）。

### 2.2 模型表与计费（model_registry / cost.h）

- 静态表：`deepseek-v4-flash`（1.008/2.016/0.02016 元/M，1M 上下文）与 `deepseek-v4-pro`（3.132/6.264/0.0261 元/M），`thinkingLevelMap = {minimal:null, low:null, medium:null, high:"high", xhigh:"max"}`。单价即人民币（¥/M）。
- `register_model` 支持运行时 override（settings.json 自定义模型场景）。
- `get_supported_thinking_levels` / `clamp_thinking_level` 精确镜像 `models.ts`：显式 `null` 标记不支持；`xhigh` 仅当有映射值；clamp 先向上后向下就近。
- `calculate_cost` = 单价/1e6 × token 数，纯函数。

### 2.3 SSE 与容错 JSON（sse_parser / json_util）

- `SseParser`：增量解析（data/event/id 字段、空行分发、CRLF、多行 data 以 `\n` 连接、注释行忽略）。可单测，fake server 依赖它构造响应。
- `repair_json`：修复字符串内的控制字符与非法转义（镜像 `repairJson`）。
- `parse_streaming_json`：完整解析 → repair 重试 → 前缀扫描回退（`longest_valid_json_prefix`：一次前向扫描收集「可能成为完整 JSON 值末尾」的位置，从右向左尝试解析；避免逐字符弹出重解析对未闭合大字符串（write 工具的流式参数）的 O(n²) 最坏情况）→ `{}` 兜底。永不抛异常。

### 2.4 TransportAdapter 接口（transport_adapter.h）

```cpp
struct StreamRequestOptions {
  std::optional<std::string> apiKey;
  std::optional<ThinkingLevel> reasoning;
  std::optional<int> maxTokens;
  std::optional<double> temperature;
  std::map<std::string, std::string> headers;
  std::shared_ptr<std::atomic<bool>> abort;  // 中断标志，贯穿 transport 与工具
  int maxRetries;
  std::chrono::milliseconds timeoutMs;
  std::string systemPrompt;
  std::vector<Tool> tools;
  std::function<void(int, const std::map<std::string,std::string>&)> onResponse;
};

class TransportAdapter {
 public:
  virtual void stream_chat(const ModelInfo&, const std::vector<Message>&,
                           const StreamRequestOptions&,
                           const std::function<void(const StreamEvent&)>& sink) = 0;
  Message complete_chat(...);  // 聚合版本：收集至 done/error
};
```

### 2.5 openai-cpp 传输（openai_transport.cpp）——wire 层核心

请求构造分四步：

1. **`detect_compat`**：由 provider/baseUrl 推导兼容设置（镜像 `detectCompat`）。deepseek 分支：`thinkingFormat="deepseek"`、`requiresReasoningContentOnAssistantMessages=true`、`supportsReasoningEffort=true`、`supportsStore=false`。
2. **`transform_messages`**（镜像 `transformMessages`）：
   - 非视觉模型图片降级为占位文本（连续图片块合并，`(image omitted: model does not support images)`）；
   - tool call id 归一化（`|` 分隔取前段、非法字符替换、40 字符截断）；
   - 跨模型时丢弃 `thoughtSignature`、thinking 转 text；
   - 跳过 `stopReason=error/aborted` 的 assistant 消息；
   - 为孤儿 tool call 补合成 `toolResult{isError:true, "No result provided"}`。
3. **`convert_messages`**（镜像 `convertMessages`）：
   - system → `system`（deepseek 不用 developer role）；
   - 单文本块 → 字符串 content；多块 → 数组（image → `data:mime;base64,...` data URL）；
   - assistant：文本拼接为字符串 content，thinking 走签名注入（见下），toolCalls → `{id, type:"function", function:{name, arguments:dump}}`；既无内容又无调用则跳过；
   - 连续 toolResult 合并为一个 `role:"tool"` 消息组（文本 `\n` 连接，纯图片用 `(see attached image)` 占位）。
4. **body augmenter**：openai-cpp 的 typed 结构无法表达 deepseek 的 `thinking` 参数与 assistant 消息的 `reasoning_content` 回放，因此在自定义 HttpClient 层解析请求体 JSON 后注入：
   - `model.reasoning && thinkingFormat=="deepseek"` → `body["thinking"] = {type: "enabled"/"disabled"}`；
   - 每个 assistant 消息按签名注入 `reasoning_content`（回放 thinking）或空串（deepseek 要求所有回放 assistant 消息携带）；
   - 有 tool_calls 无 content 的 assistant 消息 → `content: null`；
   - **注入序号不变量**：`assistantExtras[i]` 与请求体第 i 条 assistant 消息一一对应（`requiresAssistantAfterToolResult` 插入的桥接消息同样占一个槽位、推空 extras）；augmenter 超范围时跳过注入而非错配；
   - `Authorization` 头由 openai-cpp 客户端统一注入（`use_bearer_auth`），transport 不手动添加，避免重复头。

流式解析（`handle_sse_event`，镜像 openai-completions.ts 的 for-await 循环）：

- `responseId ||= chunk.id`、`responseModel ||= chunk.model`（与请求模型不同时）；
- `chunk.usage` → `parse_chunk_usage`：`input = max(0, prompt - cacheRead - cacheWrite)`、`totalTokens = 四项和`、`cost = calculate_cost`；兼容 `choice.usage` 兜底；
- `finish_reason` → `map_stop_reason`：stop/end→stop、length→length、function_call/tool_calls→toolUse、content_filter/network_error/未知→error（带 "Provider finish_reason: ..." 描述）；
- delta.content → text 块；`reasoning_content/reasoning/reasoning_text`（取第一个非空字段）→ thinking 块，signature=字段名；
- delta.tool_calls 按 index/id 累积（`byStreamIndex`/`byCallId` 双索引），`partialArgs` 累积 + `parse_streaming_json` 容错解析；
- `reasoning_details` → 匹配 toolCall 的 `thoughtSignature`；
- 每个 chunk 间检查 abort → 返回 false 停止分发 + HttpClient 中止连接；
- 无法解析的 chunk 计入 `malformedChunks`，流无 finish_reason 终止时并入最终错误信息（便于排查损坏流）；
- 流结束后按 block 顺序发 end 事件（thinking_end → toolcall_end → text_end），再检查 abort/error/无 finish_reason 三种后置条件，发 done。

**AbortableHttpClient**（自定义 `openai::HttpClient`）：

- write_callback 检查 abort atomic → 返回 0 → curl 以 `CURLE_WRITE_ERROR` 中止传输；
- 流式模式（`collect_body=false`）下仍收集 ≥400 状态码的错误响应体，保证 `throw_api_error` 带出服务端 message（如 "bad request"）；
- 自持 `curl_global_init`（绕过 openai-cpp 默认客户端的 RAII）。

## 3. agent 层

### 3.1 双队列注入（pending_queue.h）

`PendingMessageQueue`：`all` / `one-at-a-time` 两种模式，mutex 保护。注入时机与 pi 一致：

- 首个 LLM 调用前（`getSteeringMessages` 初始轮询）；
- 每个工具批次后（turn_end 之后的 steering 轮询）；
- follow-up 检查点（agent 本应停止时轮询 `getFollowUpMessages`，非空则续跑外层循环）。

### 3.2 agent 循环（agent_loop.cpp）

事件序列（对照 `agent-loop.ts`）：

```
agent_start
turn_start
[per prompt] message_start / message_end
inner loop:
  (pending steering 注入: message_start/end)
  stream_assistant_response → message_start → message_update×N → message_end
  error/aborted → turn_end + agent_end（提前终止）
  toolCalls → 工具批次 → toolResult 消息(message_start/end)
  turn_end(message, toolResults)
  prepareNextTurn（可替换 context/model/thinking）
  shouldStopAfterTurn → agent_end（提前退出，不再轮询）
  pendingMessages = getSteeringMessages()
outer loop: getFollowUpMessages → 非空则继续内层；否则 agent_end
```

`stream_assistant_response` 状态机：Start 事件把 partial message 推入 context（`addedPartial=true`），中间事件替换 `context.messages.back()`，Done/Error 用最终消息收尾；未收到终止事件时防御性补发 message_end。

### 3.3 工具批次

**preflight（串行）**：按名查工具 → `prepareArguments` shim → valijson schema 校验 → `beforeToolCall` 钩子 → abort 检查。任一失败返回 immediate error toolResult（工具未执行）。

**execute（并行，默认）**：

- 每调用一个 `std::thread`，信号量式上限 8（`atomic<int>` + condition_variable）；
- 工具线程内：execute（抛异常 = `isError` toolResult）→ `afterToolCall` 钩子（字段级覆盖 content/details/isError/terminate）；
- 流式 `onUpdate` 由工具线程收集（completion_mutex 保护），**R1 在完成序阶段统一发出** `tool_execution_end`（满足"只有 R1 推送事件"的并发约束）；
- `tool_execution_end` 按完成序，toolResult 消息按 assistant 源序（镜像 Promise.all 语义）。

**sequential 模式**：`config.toolExecution == Sequential` 或批次中任一工具 `executionMode == Sequential` → 整批串行，每步后检查 abort。

**terminate 语义**：批次所有 finalized result 的 `terminate == true` → 内层循环停止（不再发起 LLM 调用）；只要有一个为 false 则继续。

### 3.4 Agent 状态机（agent.cpp）

- `prompt`/`continue_run` 同步阻塞执行（调用线程 = run 线程），busy 时抛 `std::runtime_error`（镜像 TS 的 activeRun 检查）；
- `abort()` 设置共享 `std::atomic<bool>`，贯穿 transport（HttpClient 中止连接）与工具（execute 收到同一 signal）；
- 事件只在 run 线程派发；listeners 按订阅顺序同步调用，`subscribe` 返回的取消函数通过 alive 标志生效；
- `continue_run`：最后消息为 assistant 时先排空 steering/followUp 队列（对应 TS 的 continue 分支），否则 `run_agent_loop_continue`。

### 3.5 子 agent（subagent_tool.cpp）

- 工具 `execute` 在**父工具线程上内联**运行子 Agent（不跳线程）；
- 子 agent 工具列表 = 共享工具 + 递归 `subagent` 工具（depth+1）；
- 子 agent 流式文本 delta 经 `onUpdate` 转发为父的 `tool_execution_update`；
- abort 链：父 signal → 专用 watcher 线程（`run_finished` 标志防无限轮询）→ `sub_agent.abort()`；prompt 前已 abort 直接失败；
- 防护：`maxNestingDepth`（depth 超限返回错误）、`maxTurns`（`shouldStopAfterTurn` 钩子计数，达到即停；若最后一轮已给出干净的最终回答则照常返回，仅当最后一条 assistant 消息仍含工具调用——工作被截断——时才报超限）；
- 结果包装为 `<subagent>\n{最终 assistant 文本}\n</subagent>`（排除 error/aborted 消息）。

## 4. harness 层

### 4.1 会话存储（session.h / jsonl_repo.h）

JSONL 格式（pi 兼容 version-3）：

```
{"type":"session","version":3,"id":"...","timestamp":"ISO8601","cwd":"...","parentSession":"..."}   ← 头部
{"type":"message","id":"8位","parentId":null,"timestamp":"...","message":{role,content[],usage,stopReason,...}}
{"type":"thinking_level_change","thinkingLevel":"high"}
{"type":"model_change","provider":"deepseek","modelId":"deepseek-v4-flash"}
{"type":"active_tools_change","activeToolNames":[...]}
{"type":"compaction","summary":"...","firstKeptEntryId":"...","tokensBefore":123,"details":{...},"fromHook":false}
{"type":"branch_summary","fromId":"...","summary":"..."}
{"type":"custom"/"custom_message"/"label"/"session_info"/"leaf"}
```

- `JsonlSessionStorage`：append-only；`currentLeafId` 跟踪（leaf 条目指向当前分支）；`get_path_to_root` 沿 parentId 回溯（父链不存在报 invalid_session）；条目 id 为 uuidv7 前 8 位（冲突重试，uuidv7 内部状态串行化）。
- **线程安全**：所有公开方法由内部 mutex 保护；`append_child` 在单次加锁内完成「生成 id → 挂到当前 leaf → 落盘」。run 线程（消息持久化）与 UI 线程（/model、/thinking、/tools 切换）并发追加时历史链保持线性，不会产生孤儿分支或交错写坏 JSONL。
- **尾行损坏自愈**：进程在追加中途被强杀（Ctrl+C/SIGKILL）会在文件尾留下半行 JSON。`open()` 对最后一条非空行解析失败做截断修复（写回最后一个完整行）；文件中部的损坏仍然拒绝打开。
- 目录布局：`~/.pi-cpp/agent/sessions/<--编码后cwd-->/<ts>_<id>.jsonl`，cwd 编码为 `--` + 路径去前导 `/`、`/:\` 换 `-` + `--`。
- `Session::build_context` 重放：thinking_level/model（model_change 与 assistant 消息都能更新 model）/active_tools 扫描；compaction 条目存在时用摘要消息替换压缩前历史（`firstKeptEntryId` 起保留）。

### 4.2 压缩（compaction.cpp）

- `estimate_tokens`：chars/4（text/thinking/toolCall 参数 JSON），图片块固定 4800；`estimate_context_tokens` 用**最近一条成功 assistant 的 usage** 作基准 + 其后消息估算（镜像 estimateContextTokens）；
- `should_compact = enabled && tokens > contextWindow - reserveTokens`（默认 reserve=16384，keepRecent=20000）；
- `find_cut_point`：从尾部累积到 `keepRecentTokens`，切点取合法边界（user/assistant 消息、branch_summary/custom_message）；切点前若是 toolResult 等非边界则前移；切点非 user 消息时检测 split-turn（回溯最近的 user 起始）；
- `prepare_compaction`：跳过上次压缩后的边界、继承上次的 `readFiles/modifiedFiles` details、提取历史中的 read/write/edit 工具参数（路径）；无可摘要消息（如新会话只有 model_change 等条目）时返回空 preparation，调用方报 "Nothing to compact" 而不是对空对话发起 LLM 调用；
- `compact`：LLM 生成摘要（`SUMMARIZATION_SYSTEM_PROMPT` + 结构化格式，带 `<previous-summary>` 增量更新），split-turn 时附加 turn-prefix 摘要；追加 `<read-files>/<modified-files>` 标签；
- `AgentHarness::run_compaction`：prepare → generate → `session.append_compaction` → 用 `build_context` 重放结果替换 agent 消息。

### 4.3 skills / 模板

- `load_skills`：递归遍历目录，优先 `SKILL.md`，根目录 `.md` 也加载；frontmatter 简化 YAML 解析（`key: value`、引号、`disable-model-invocation: true`）；name 校验（小写 a-z/0-9/连字符、与父目录名一致、≤64）；无 description 的 skill 丢弃并产生诊断。
- `format_skills_for_system_prompt`：`<available_skills>` XML（name/description/location，XML 转义）。
- `parse_command_args`：shell 风格引号解析；`substitute_args`：`$1`/`$@`/`$ARGUMENTS`/`${@:N}`/`${@:N:L}`（越界为空）。

### 4.4 env（POSIX）

- `PosixFileSystem`：所有操作返回 `Result<T, FileError>` 不抛异常；`resolve` 相对 cwd；`absolute_path` 语法归一化（不解析符号链接）；错误码区分 not_found/permission/not_directory 等。
- `PosixShell::exec`：fork/exec `/bin/sh -c`，stdout/stderr 管道**非阻塞 + poll(100ms)**，abort → SIGKILL，timeout 秒级；流式回调 `onStdout/onStderr`；`options.env` 覆盖在 fork 前构造 envp（fork 后至 exec 之间只允许 async-signal-safe 调用，不能在子进程 setenv/malloc），经 `execle` 传入。

### 4.5 AgentHarness

- 构造：Agent + Session + 工具 + skills + transport；订阅 agent 事件 → `message_end` 持久化 + 成本累计（`total_cost_` 由 mutex 保护：run 线程写、UI 线程读）。
- `resume`：`build_context` 恢复 model（`get_model(modelId)` 校验）/thinking/active tools/messages。
- `set_model`/`set_thinking_level`/`set_tools`：持久化 `model_change`/`thinking_level_change`/`active_tools_change` 条目。
- 自动压缩：`prompt` 返回后估算会话 token，超阈值触发 `compact`。

## 5. app 层

### 5.1 settings + .env

`~/.pi-cpp/settings.json`：

```json
{
  "apiKey": "...", "baseUrl": "https://api.deepseek.com",
  "model": "deepseek-v4-flash", "thinking": "off",
  "byCwd": { "/path/to/project": {"model": "deepseek-v4-pro", "thinking": "high"} }
}
```

apiKey 优先级：环境变量 `PI_API_KEY` > `DEEPSEEK_API_KEY` > 项目根 `.env` > `~/.pi-cpp/.env` > settings.json。`.env` 解析支持注释/引号/空白。`PI_BASE_URL`/`PI_MODEL` 环境变量覆盖端点与模型（测试与切换端点用）。模型选择按 cwd 持久化（`set_override` 写 `byCwd`），重启后 `resume` 恢复。

### 5.2 图片（images.cpp）

stb_image 解码（强制 RGBA）→ 最长边 >2000px 时 stb_image_resize2 等比缩放 → stbi_write_png 编码 → base64。5MB 上限。deepseek 模型不支持图片，transport 层自动降级为占位文本（见 2.5）。

### 5.3 REPL（repl.cpp）

- termios raw mode（进入前保存完整终端设置，退出时原样恢复，不重建近似值；退出用 TCSANOW——macOS 上 TCSAFLUSH/TCSADRAIN 会等待 pty 输出队列排空，主端无读者时永久阻塞在 ioctl），`poll(stdin, event_pipe)` 双路循环；worker 线程跑 `AgentSession::prompt`，事件经 mutex 队列 + pipe 单字节通知送达 UI 线程渲染（UI 线程独占终端写入）。
- **键盘协议**：进入 raw mode 时发送 `CSI > 1 u` 启用 CSI-u/kitty 键盘协议（iTerm2/kitty/WezTerm 支持；不支持的终端忽略并继续发传统字节流），退出时 `CSI < u` 弹栈。按键统一经 `decode_key_event` 解码为 KeyEvent：兼容传统字节（CR/LF、BS、ESC [ 序列）与 CSI-u（`<codepoint>[;<modifiers>]u`）；Ctrl+C 等修饰键在两种编码下都映射一致。Shift+Enter（`13;1u`/`13;2u`）、Ctrl+J（`106;4u`）、Option+Enter（`ESC CR`）三种编码都映射为换行。
- **命令菜单**：缓冲区首个字符为 `/` 时实时渲染匹配命令（内建命令表 + 用户模板，`builtin_command_entries()` 单一来源，与 /help 共用）；Tab 补全到最长公共前缀（唯一匹配补全并加空格）；最多显示 8 条。
- **多行输入**：Shift+Enter/Ctrl+J/Option+Enter 插入 `\n`；渲染与光标定位按「行 + 显示宽度」计算（`display_width` 近似 wcwidth，中文/全角/emoji 计 2 列，无 locale 依赖）；提交时保持输入区可见、输出从输入区与菜单之下开始。
- 行编辑：逐字符回显、Backspace（按 UTF-8 序列删除）、左右方向键、Home/End、Ctrl+L 重绘；Esc 序列后续字节带超时读取（裸按 Esc 不再挂起 UI）。
- 流式快捷键：Enter=steer（整行入队）、Esc=abort、Ctrl+C=退出（二次确认，流式中始终生效）。
- **Ctrl+C 退出路径**：abort → kill 子进程组 → **先恢复终端再 join worker**。worker 尚未收尾时提示「等待运行中的任务结束，Ctrl+C 可立即退出」——终端已回到 cooked 模式，此后的 Ctrl+C 以 SIGINT 直接终止进程，不会被卡在 raw mode（raw mode 下 ISIG 关闭，Ctrl+C 是死键）。
- 空闲快捷键：Ctrl+P=循环切换模型（flash↔pro）、Ctrl+C=退出。
- 状态栏：`[model=..] [thinking=..] [turn=¥..] [all=¥..] [mem <驻留内存> peak=..]`（turn/all 与 `calculate_cost` 同源，人民币计费；内存只显示驻留内存）。内存指标针对实际运行的 `picpp` 进程；Linux 读取 `/proc/self/status` 的 `VmRSS`/`VmSize`；macOS 用 `task_vm_info`——`rss` 取 `phys_footprint`（与「活动监视器」同口径，含压缩页），`vms` 取 `virtual_size`（Apple Silicon 的稀疏映射可达数百 GiB，数值大不代表实际占用）。
- slash 命令：/help /model /thinking /compact /clear /new /resume /sessions /image /tools /skills /memory /quit + 用户模板（`~/.pi-cpp/templates/<name>.md` → `/<name> 参数`）。
- 管道模式（非 TTY 或带参数）：`picpp "prompt" [--image path]`，订阅事件流式打印 + usage 行；退出前 kill 全部子进程组，避免 bash 工具遗留孤儿进程。

## 6. 并发模型

| 线程 | 运行 | 阻塞点 |
|---|---|---|
| UI（主线程） | REPL poll 循环 | `poll(stdin, event_pipe)`，从不阻塞 agent 工作 |
| Run 线程 R1（REPL 每次 prompt 新建） | `Agent::prompt` → `run_agent_loop` | transport 内 HTTP、工具 join |
| 工具线程 T1..Tn（每批次，上限 8） | `AgentTool::execute` | 工具工作、子 agent |
| 子 agent | 父工具线程内联（不跳线程） | 自身 HTTP |

- **事件通道**：mutex+condvar deque + pipe 通知字节；只有 R1 推送事件（工具事件由循环收集后从 R1 发出）。
- **abort 传播链**：`Agent::abort()` → 共享 `atomic<bool>` → transport write_callback（中止连接）+ 工具 execute（轮询检查；bash 每 100ms poll 检查并 SIGKILL 进程组，read/grep/find 在遍历循环中检查并返回部分结果）+ 子 agent（watcher 桥接）。
- **死锁规避**：线程按调用分配（无共享有界线程池 → 无池饥饿）；嵌套深度/轮次上限；子 agent 状态隔离（仅共享 abort atomic）。
- **锁与共享状态清单**：`JsonlSessionStorage::mutex_`（会话条目/索引）、`uuidv7` 内部互斥（条目 id 生成）、`AgentHarness/AgentSession::cost_mutex_`（成本累计）、`Agent::state_mutex_`（状态快照）、`PendingMessageQueue` 自带互斥。规则：跨线程共享状态要么加锁，要么只在 run 线程写。
- **tsan 验证**：全套 118 用例（含多线程会话并发追加回归测试）在 `-fsanitize=thread` 下 0 警告。

## 7. Wire 协议细节（DeepSeek）

| 特性 | 发送 | 接收 |
|---|---|---|
| 流式 | `stream:true` + `stream_options.include_usage` | 末 chunk usage |
| thinking | `thinking:{type:"enabled"/"disabled"}` + `reasoning_effort`（thinkingLevelMap 映射，如 high→"high"、xhigh→"max"） | `reasoning_content` delta → thinking 块 |
| thinking 回放 | assistant 消息 `reasoning_content:<joined thinking>` | — |
| 工具 | `tools:[{type:"function",function:{name,description,parameters,strict:false}}]` | delta.tool_calls 按 index 累积 |
| 图片 | 非视觉模型降级为占位文本（不发 image_url） | — |
| 计费 | — | `prompt_tokens_details.{cached_tokens,cache_write_tokens}` → cacheRead/cacheWrite |

## 8. 与 pi 的映射关系

| pi (TS) | pi-cpp |
|---|---|
| `packages/ai/src/types.ts` | `include/pi/ai/types.h`, `events.h` |
| `packages/ai/src/models.ts` + models.generated.ts | `model_registry.h/cpp`（deepseek 子集） |
| `packages/ai/src/providers/openai-completions.ts` | `src/ai/openai_transport.cpp` |
| `packages/ai/src/providers/transform-messages.ts` | `transform_messages`（同文件） |
| `packages/ai/src/utils/json-parse.ts` | `src/ai/json_util.cpp` |
| `packages/ai/src/utils/event-stream.ts` | sink 回调（无异步流对象） |
| `packages/agent/src/agent.ts` | `include/pi/agent/agent.h` + `src/agent/agent.cpp` |
| `packages/agent/src/agent-loop.ts` | `src/agent/agent_loop.cpp` |
| `packages/agent/src/harness/session/jsonl-storage.ts` | `src/harness/session.cpp`（JsonlSessionStorage） |
| `packages/agent/src/harness/session/session.ts` | `Session`（同文件） |
| `packages/agent/src/harness/session/jsonl-repo.ts` | `src/harness/jsonl_repo.cpp` |
| `packages/agent/src/harness/compaction/compaction.ts` | `src/harness/compaction.cpp` |
| `packages/agent/src/harness/compaction/utils.ts` | 同文件（file ops/serializeConversation） |
| `packages/agent/src/harness/skills.ts` | `src/harness/skills.cpp` |
| `packages/agent/src/harness/prompt-templates.ts` | `src/harness/prompt_templates.cpp` |
| `packages/agent/src/harness/system-prompt.ts` | `src/harness/system_prompt.cpp` |
| `packages/agent/src/harness/agent-harness.ts` | `src/harness/agent_harness.cpp`（裁剪版） |
| `packages/agent/src/harness/messages.ts` | 常量内联于 compaction/session |

## 9. 测试策略

| 测试 | 手法 | 对照 |
|---|---|---|
| cost_test | 纯函数断言（含 deepseek 真实价格） | cost 计算 |
| sse_parser_test | 分帧/CRLF/多行/增量/注释 | — |
| transport_test | **fake_sse_server**（POSIX socket，脚本化 SSE 帧 + 逐帧延迟）+ 请求体断言 | openai-completions wire 格式 |
| agent_loop_test | **ScriptedTransport**（按序回放脚本回合） | agent-loop.test.ts 场景（事件序列、并行/串行、terminate、steer/followUp、钩子、abort） |
| session_test | 真实临时目录 round-trip | jsonl-storage/session 行为 |
| compaction_test | 纯函数（估算/切点/文件操作） | compaction.test.ts 核心 |
| skills_test / templates_test | 临时目录加载 + 占位符替换 | skills/prompt-templates 行为 |
| subagent_tool_test | 脚本化 transport + 真实线程 | 嵌套/轮次/深度/abort 链 |

测试设施：`tests/test_utils/fake_sse_server.h`（脚本/处理器双模式、请求记录）、`tests/test_utils/scripted_transport.h`。

## 10. 已知裁剪与差异

0. **平台范围**：macOS 与 Linux（POSIX）。Windows 原生不支持——REPL（termios/poll）与 Shell（fork/exec、/bin/sh）依赖 POSIX，Windows 用户请用 WSL；`FileSystem`/`Shell` 已是接口（env.h），未来若需原生 Windows 支持可新增 `WindowsShell`/Console 后端而不动上层。
1. **单 provider**：仅 OpenAI-completions wire（deepseek 端点），无 OAuth/浏览器代理/多 provider/图片生成。
2. **传输实现**：openai-cpp 库（而非自写 curl SSE），`thinking`/`reasoning_content` 等 deepseek 专有字段经 body augmenter 注入。
3. **AgentHarness 裁剪**：无完整事件 hook 系统（before_agent_start/before_provider_request 等），保留消息持久化 + 自动压缩 + 模型/thinking 恢复。
4. **REPL 简化**：无补全/历史；流式时普通字符输入不缓冲（只接受 Enter/Esc/Ctrl+C）。
5. **compaction 简化**：split-turn 的 turn-prefix 摘要复用主摘要生成函数；无 branch summarization 全流程。
6. **skills 忽略文件**：不实现 .gitignore 匹配（目录遍历固定排除 .git/build/node_modules）。
7. **bash 工具**：POSIX `/bin/sh`，无伪终端/交互式命令支持；支持 `options.env` 环境覆盖。**无人工确认审批**（pi 的审批流未移植）：agent 自主执行任意 shell 命令（cwd 限定在 workspace 内，但命令内容不受限），仅建议个人终端使用。

## 11. 构建与验证

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/pi_tests                     # 127 用例
./build/picpp                        # 交互 REPL
./build/picpp "prompt"               # 管道一次性对话（自动读 .env）

# ThreadSanitizer（网络受限时可用 FETCHCONTENT_SOURCE_DIR_* 复用 build/_deps 已下载的依赖源；
# stb 已 vendor 在 third_party/stb/，无需拉取）
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=build/_deps/nlohmann_json-src \
  -DFETCHCONTENT_SOURCE_DIR_OPENAI-CPP=build/_deps/openai-cpp-src \
  -DFETCHCONTENT_SOURCE_DIR_VALIJSON=build/_deps/valijson-src \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=build/_deps/googletest-src
cmake --build build-tsan && ./build-tsan/pi_tests
```

代码风格：`.clang-format`（Google 基础 + Allman 大括号 + 4 空格缩进 + 100 列 + include regroup），`clang-format -i $(find include src apps tests -name '*.cpp' -o -name '*.hpp' -o -name '*.h')`。

**CI**：`.github/workflows/ci.yml` 在 push/PR 时跑 macOS 14 与 Ubuntu 24.04 双平台矩阵（构建 + 单测 + tsan 单测）。Linux 平台以此为准；本地改动若涉及 env/repl/commands 等平台相关代码，请确认 CI 两个平台都绿再合并。

## 12. 维护指南（人工维护必读）

### 12.1 必须保持的不变量

1. **事件只从 run 线程派发**：并行工具线程的流式 update 先收集（completion_mutex），由 run 线程在完成序阶段统一发出（agent_loop.cpp `execute_tool_calls_parallel`）。新增任何异步事件源都必须沿用这一约束，否则 UI 终端渲染会交错。
2. **会话存储线程安全**：`JsonlSessionStorage` 的所有公开方法都假设被 UI 线程与 run 线程并发调用。新增方法必须持有内部 mutex；「读 leaf + 追加」的复合操作必须走 `append_child`（单次加锁），拆成多个独立加锁调用会产生孤儿分支。对象经 shared_ptr 共享、只允许移动（拷贝被禁用，见 session.h 注释）。
3. **`assistantExtras` 序号对应**：`convert_messages` 生成的 extras 必须与请求体 assistant 消息一一对应（含桥接消息）。在 convert 或 augmenter 里增删 assistant 消息时，必须同步维护 extras，否则 deepseek 的 `reasoning_content` 回放会错位。
4. **`stream_chat` 契约**：不得抛异常；一切失败以 `kError` 事件终止。任何改动不得在 sink 调用之外泄漏异常。
5. **成本/用量统计**：累计值写入必须持 `cost_mutex_`（AgentHarness 与 AgentSession 各有一把）。读取同样加锁。
6. **行为规范是 pi 的 vitest 套件**：agent_loop / compaction / session / transport 的语义对照 pi 对应测试，改动行为时同步更新本仓库对应测试与本文档。
7. **工具执行不得抛异常**：工具在 agent 循环中无 try/catch 包裹，异常会冒泡到 `Agent::run` 的兜底 catch，直接终结整轮对话。参数解析（尤其 LLM 传入的 `get<type>()`）必须自行兜底为错误文本——web_fetch 的整体 try/catch 是范例。

### 12.2 新增 provider / 模型 / 工具 / 命令

| 改动 | 位置 |
|---|---|
| 内置模型 | `src/ai/model_registry.cpp` 的 `build_builtin_models()`；运行时 override 用 `register_model` |
| provider 兼容分支 | `src/ai/openai_transport.cpp` 的 `detect_compat`（镜像 pi detectCompat，同步 §2.5/§7 wire 表格） |
| 编码工具 | `src/app/commands.cpp` 的 `make_coding_tools`（schema 由 valijson 自动校验；`executionMode=Sequential` 可强制串行）。web_fetch 走 libcurl，详见 §12.1 第 7 条契约 |
| slash 命令 | `src/app/repl.cpp` 的 `handle_command`（按现有 if 链追加）；用户模板放 `~/.pi-cpp/templates/<name>.md` |
| 会话 JSONL 格式 | `src/harness/session.cpp`（version-3 头部；格式变更必须兼容旧文件，`session_entry_from_json` 负责解析） |

### 12.3 提交前检查清单

1. `cmake --build build && ./build/pi_tests` 全绿（当前 127 用例；另有 1 个真实联网用例默认跳过，`PI_LIVE_NET_TESTS=1` 启用）。
2. REPL 交互改动（输入解码/渲染/退出路径）必须过 pty 冒烟：菜单唤起、Tab 补全、Shift+Enter/Ctrl+J 多行、空闲 Ctrl+C 干净退出（见 `tests/` 之外的手动清单，pty 脚本驱动）。
2. 涉及并发/线程改动：跑一遍 tsan（见 §11）。
3. `clang-format -i` 改动的文件。
4. 若行为语义有变：更新对应测试、README 与本文档（用例数、wire 表格、已知裁剪清单）。

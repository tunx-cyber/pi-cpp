# pi-cpp

pi（TypeScript 终端 Agent）的 C++ 移植版。最核心的 agent 功能：流式对话、工具调用、模型切换、多 agent（子 agent 嵌套）、会话持久化/恢复、上下文压缩、skills、slash 命令。

## 平台支持

| 平台 | 状态 |
|---|---|
| macOS（arm64/x86_64） | ✅ 开发与验证平台（单测 + tsan + REPL 实测） |
| Linux | ✅ CI 验证（GitHub Actions：Ubuntu 24.04 构建 + 单测 + tsan） |
| Windows | ❌ 原生不支持（REPL/Shell 依赖 POSIX）。请使用 **WSL** 运行 |

## 构建

依赖：CMake ≥ 3.16、Ninja、clang、libcurl。googletest 由 FetchContent 自动下载；nlohmann-json、stb 已 vendor 在 `third_party/`（纯 header，无需下载）；工具参数 JSON Schema 校验为项目内置轻量实现（不依赖 valijson）。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 使用

```bash
# 交互式 REPL
./build/picpp

# 一次性对话（管道模式）
./build/picpp "用一句话介绍你自己"
echo "hello" | ./build/picpp

# 图片输入（deepseek 当前不支持图片，自动降级为占位文本）
./build/picpp "这是什么" --image photo.png

# 恢复到指定历史会话继续对话（id 支持前缀；不带 id 恢复最近会话）
./build/picpp --resume <id> "继续刚才的任务"
```

API key 自动从以下位置读取（优先级从低到高）：`~/.pi-cpp/.env`、当前目录向上最近的项目 `.env`（`DEEPSEEK_API_KEY` / `OPENAI_API_KEY`）、环境变量 `DEEPSEEK_API_KEY` / `OPENAI_API_KEY` / `PI_API_KEY`。端点/模型可用 `PI_BASE_URL` / `PI_MODEL` 覆盖。

REPL 命令：`/help /model /thinking /compact /clear /new /resume /sessions /image /tools /skills /quit`。

- `/sessions` 列出历史会话（序号 / 时间 / 首条消息预览 / 消息数，标出当前会话）
- `/resume` 恢复最近会话；`/resume <序号>` 或 `/resume <id 前缀>` 恢复到指定会话（类似 `claude --resume`）

- 输入 `/` 唤起命令菜单（实时过滤 + Tab 补全）
- 输入：`Enter`=提交，`Shift+Enter`/`Ctrl+J`/`Option+Enter`=换行（多行输入；Shift+Enter 需终端支持 CSI-u/kitty 键盘协议：iTerm2/kitty/WezTerm）
- 流式中：`Enter`=steer（排队下一句）、`Esc`=abort、`Ctrl+C`=退出（确认）、`Ctrl+P`=切模型、`Ctrl+L`=重绘
- `/tools` 启用编码工具：read / bash / edit / write / grep / find / ls / web_fetch / web_search + subagent（子 agent 嵌套）。web_search 走 DeepSeek Anthropic 兼容 Messages API（原生 `web_search_20250305`），需 `DEEPSEEK_API_KEY`。**注意：bash 工具无人工确认审批**，agent 会自主执行 shell 命令，仅建议个人终端使用
- 会话保存在 `~/.pi-cpp/agent/sessions/<cwd>/<ts>_<id>.jsonl`（pi 兼容 version-3 格式），重启后 `/resume` 恢复
- 用户模板：`~/.pi-cpp/templates/<name>.md` → `/<name> 参数`（支持 `$1`/`$@`/`${@:N:L}`）

## 架构

分层镜像 pi：`ai → agent → harness → app(REPL)`，每层只依赖下一层。完整技术细节（wire 协议、并发模型、会话格式、与 pi 的映射、测试策略）见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

```
include/pi/
├── ai/       Json 别名 + TransportAdapter 接口（唯一知道 HTTP/SSE 的层）
│             openai_transport：内置轻量 openai 客户端实现（流式 SSE/usage/thinking/工具调用/abort）
│             model_registry：deepseek-v4-flash / deepseek-v4-pro + 运行时 override
│             cost：价格/1e6 × token（纯函数）；sse_parser：独立 SSE 帧解析
├── agent/    Agent（prompt/continue/steer/followUp/abort/reset/subscribe/waitForIdle）
│             agent_loop（双队列注入时机、串/并行工具批次、钩子、terminate 语义）
│             json_schema（内置轻量 JSON Schema 校验）、subagent_tool（嵌套/事件转发/abort 链/深度轮次防护）
├── harness/  Session（JSONL version-3，leaf 跟踪）+ JsonlSessionRepo
│             compaction（切点/估算/摘要）、skills（SKILL.md）、prompt_templates
│             AgentHarness（消息持久化 + 自动压缩 + 模型/thinking 恢复）
│             env（POSIX FileSystem + Shell，abort 感知）
└── app/      settings（按 cwd 持久化）、images（stb ≤2000px resize + base64）
              commands（slash + 编码工具）、repl（raw mode + 事件泵 + 状态栏 + 进程内存监控）
```

并发模型：阻塞 + 线程池。agent 循环单线程阻塞执行；每个工具批次创建最多 8 个工作线程并从队列领取任务；子 agent 在父工具线程内联（不跳线程）。前置/后置钩子和事件只在 run 线程执行，经 mutex+pipe 送到 UI 线程。

## 测试

```bash
./build/pi_tests                # 真实联网用例默认跳过
# ThreadSanitizer（可复用已下载的 googletest，见 docs/TECHNICAL.md §11）
cmake -S . -B build-tsan -G Ninja -DPI_SANITIZER=thread
cmake --build build-tsan && ./build-tsan/pi_tests

# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -G Ninja -DPI_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

# 修改文件的格式检查（clang-format 18）
python3 scripts/check_format.py
```

测试对照 pi 的 vitest 套件：`cost_test`、`sse_parser_test`、`transport_test`（fake SSE server + 录制 fixture）、`agent_loop_test`（脚本化 transport）、`session_test`/`compaction_test`/`skills_test`/`templates_test`、`subagent_tool_test`。

## 里程碑状态

M1 骨架+transport ✅ · M2 ai 层完整 ✅ · M3 agent 循环+工具 ✅ · M4 harness ✅ · M5 REPL 应用 ✅ · M6 子 agent+打磨 ✅

项目代码默认启用 `-Wall -Wextra -Wpedantic`；CI 使用 `-DPI_WARNINGS_AS_ERRORS=ON`。
`Result` 标记为 `[[nodiscard]]`，调用方必须检查失败结果。会话存储失败会向运行/UI 边界报告；
模型、thinking 与工具设置在持久化成功后才更新运行状态。

工具钩子的 `context` 为只读视图；后置钩子的 `content/details/isError/terminate` 使用
`std::optional` 表达字段覆盖，未设置表示保留原值。需要替换上下文时使用 `prepareNextTurn` 返回值。

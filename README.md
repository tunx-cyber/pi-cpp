# pi-cpp

pi（TypeScript 终端 Agent）的 C++ 移植版。最核心的 agent 功能：流式对话、工具调用、模型切换、多 agent（子 agent 嵌套）、会话持久化/恢复、上下文压缩、skills、slash 命令。

## 构建

依赖：CMake ≥ 3.16、Ninja、clang、libcurl。第三方库（nlohmann-json、openai-cpp、valijson、stb、googletest）由 FetchContent 自动下载。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 使用

```bash
# 交互式 REPL
./build/pi_repl

# 一次性对话（管道模式）
./build/pi_repl "用一句话介绍你自己"
echo "hello" | ./build/pi_repl

# 图片输入（deepseek 当前不支持图片，自动降级为占位文本）
./build/pi_repl "这是什么" --image photo.png
```

API key 自动从以下位置读取（优先级从低到高）：`~/.pi-cpp/.env`、项目根 `.env`（`DEEPSEEK_API_KEY`）、环境变量 `DEEPSEEK_API_KEY` / `PI_API_KEY`。端点/模型可用 `PI_BASE_URL` / `PI_MODEL` 覆盖。

REPL 命令：`/help /model /thinking /compact /clear /new /resume /sessions /image /tools /skills /quit`。

- 流式中：`Enter`=steer（排队下一句）、`Esc`=abort、`Ctrl+C`=退出（确认）、`Ctrl+P`=切模型、`Ctrl+L`=重绘
- `/tools` 启用编码工具：read / bash / edit / write / grep / find / ls + subagent（子 agent 嵌套）
- 会话保存在 `~/.pi-cpp/agent/sessions/<cwd>/<ts>_<id>.jsonl`（pi 兼容 version-3 格式），重启后 `/resume` 恢复
- 用户模板：`~/.pi-cpp/templates/<name>.md` → `/<name> 参数`（支持 `$1`/`$@`/`${@:N:L}`）

## 架构

分层镜像 pi：`ai → agent → harness → app(REPL)`，每层只依赖下一层。完整技术细节（wire 协议、并发模型、会话格式、与 pi 的映射、测试策略）见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

```
include/pi/
├── ai/       Json 别名 + TransportAdapter 接口（唯一知道 HTTP/SSE 的层）
│             openai_transport：openai-cpp 实现（流式 SSE/usage/thinking/工具调用/abort）
│             model_registry：deepseek-v4-flash / deepseek-v4-pro + 运行时 override
│             cost：价格/1e6 × token（纯函数）；sse_parser：独立 SSE 帧解析
├── agent/    Agent（prompt/continue/steer/followUp/abort/reset/subscribe/waitForIdle）
│             agent_loop（双队列注入时机、串/并行工具批次、钩子、terminate 语义）
│             json_schema（valijson 校验）、subagent_tool（嵌套/事件转发/abort 链/深度轮次防护）
├── harness/  Session（JSONL version-3，leaf 跟踪）+ JsonlSessionRepo
│             compaction（切点/估算/摘要）、skills（SKILL.md）、prompt_templates
│             AgentHarness（消息持久化 + 自动压缩 + 模型/thinking 恢复）
│             env（POSIX FileSystem + Shell，abort 感知）
└── app/      settings（按 cwd 持久化）、images（stb ≤2000px resize + base64）
              commands（slash + 编码工具）、repl（raw mode + 事件泵 + 状态栏）
```

并发模型：阻塞 + 线程池。agent 循环单线程阻塞执行；工具批次每调用一个线程（上限 8）；子 agent 在父工具线程内联（不跳线程）。事件只在 run 线程派发，经 mutex+pipe 送到 UI 线程。

## 测试

```bash
./build/pi_tests                # 97 个用例
# ThreadSanitizer（tsan 清零）
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ./build-tsan/pi_tests
```

测试对照 pi 的 vitest 套件：`cost_test`、`sse_parser_test`、`transport_test`（fake SSE server + 录制 fixture）、`agent_loop_test`（脚本化 transport）、`session_test`/`compaction_test`/`skills_test`/`templates_test`、`subagent_tool_test`。

## 里程碑状态

M1 骨架+transport ✅ · M2 ai 层完整 ✅ · M3 agent 循环+工具 ✅ · M4 harness ✅ · M5 REPL 应用 ✅ · M6 子 agent+打磨 ✅

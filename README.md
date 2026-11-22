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

### API key 配置

API key 按以下优先级自动解析（`src/app/settings.cpp` 的 `Settings::load()`）：

| 优先级 | 来源 |
|---|---|
| 1（最高） | 环境变量 `PI_API_KEY` |
| 2 | 环境变量 `DEEPSEEK_API_KEY` |
| 3 | 环境变量 `OPENAI_API_KEY` |
| 4 | 项目 `.env`（从 cwd 向上逐级找最近的，变量名同上） |
| 5 | `~/.pi-cpp/.env`（全局） |

每个变量名内部依次查「进程环境变量 → 项目 `.env` → `~/.pi-cpp/.env`」；`.env` 解析支持 `#` 注释与引号。**API key 与全局端点不读 settings.json**：端点只来自 `PI_BASE_URL`（默认 `https://api.deepseek.com`，见 `settings.h`），模型可用 `PI_MODEL` 覆盖。旧 `settings.json` 里的 `apiKey` / `baseUrl` 键会被忽略，并在下次保存时移除。

请求链路：key 注入传输层后，发送时放入 HTTP 头 `Authorization: Bearer <key>`；context compaction 与子 agent 通过 `getApiKey` 回调按需获取；DeepSeek `web_search` 工具会同时发 `x-api-key` 与 `Authorization` 两个头（兼容官方端点与 Anthropic 兼容代理）。若最终为空，请求直接报错提示设置上述变量。`settings.json` 不再保存凭据，写回权限仍为 `0600`（兼容旧文件中可能残留的凭据）。

REPL 命令：`/help /model /thinking /compact /clear /new /resume /sessions /image /tools /skills /memory /cost /context /quit`。

- `/sessions` 列出历史会话（序号 / 时间 / 首条消息预览 / 消息数，标出当前会话）；流式扫描元数据，不把整份会话读进内存
- `/resume` 恢复最近会话；`/resume <序号>` 或 `/resume <id 前缀>` 恢复到指定会话（类似 `claude --resume`）
- `/model` 无参时请求 OpenAI 兼容端点的 `GET {baseUrl}/models`，用 ↑/↓ 选择、Enter 确认、Esc 取消；`/model <id>` 直接切换，`Ctrl+P` 循环切换
- `/thinking` 无参时同样用 ↑/↓ 选择（off/minimal/low/medium/high/xhigh），`/thinking <级别>` 直接设置；不做模型能力预校验，真实请求失败会把错误显示出来
- 默认模型不写死：启动时若没有已配置/已保存的模型，取 `GET /models` 返回的第一个模型；端点不可达时状态栏显示 `[model=未选择]`，选好模型前不会发送请求
- 端点只返回模型 id：`deepseek-*` 等远端 id 会自动继承同族模型的能力（reasoning / thinking 映射 / 上下文窗口），因此 `/thinking` 对这类模型仍可用（off/high/xhigh）
- `/cost` 显示当前会话、最近一次调用与进程累计的 input / output / cacheRead / cacheWrite / total token 数。单价无法确定，因此只记录 token，不显示金额
- `/context` 显示当前上下文的 token 估算（字符数/4，图片按 4800）与模型上限占用百分比
- `/compact` 手动压缩上下文（worker 线程执行，压缩期间 Esc 可取消）；**不再自动压缩**，长会话由 `/context` 观察、需要时手动压缩
- 启动默认**新会话**；`--resume [id]` 或 `/resume` 恢复历史，恢复后会回放完整对话
- 主题：`settings.json` 的 `"theme": "default" | "mono"`，`NO_COLOR=1` 自动切到无颜色

- 输入 `/` 唤起命令菜单（实时过滤）：`↑`/`↓` 选择、`Tab` 补全、`Enter` 执行
- 输入：`Enter`=提交，`Shift+Enter`/`Ctrl+J`/`Option+Enter`=换行（多行输入；Shift+Enter 需终端支持 CSI-u/kitty 键盘协议：iTerm2/kitty/WezTerm）
- 流式中：`Enter`=steer（排队下一句）、`Esc`=abort、`Ctrl+C`=退出（确认）、`Ctrl+P`=切模型、`Ctrl+L`=重绘
- 编码工具默认启用：read / bash / edit / write / grep / find / ls / web_fetch / web_search + subagent（子 agent 嵌套），`/tools` 可重新加载。web_search 走 DeepSeek Anthropic 兼容 Messages API（原生 `web_search_20250305`），需 `DEEPSEEK_API_KEY`。**注意：bash 工具无人工确认审批**，agent 会自主执行 shell 命令，仅建议个人终端使用
- skills：`~/.pi-cpp/skills/**/SKILL.md` 启动时加载并写入 system prompt（模型按 location 自行读取）；`/skills` 查看清单
- 会话保存在 `~/.pi-cpp/agent/sessions/<cwd>/<ts>_<id>.jsonl`（pi 兼容 version-3 格式），重启后 `/resume` 恢复
- 用户模板：`~/.pi-cpp/templates/<name>.md` → `/<name> 参数`（支持 `$1`/`$@`/`${@:N:L}`）

## 架构

分层镜像 pi：`ai → agent → harness → app(REPL)`，每层只依赖下一层。完整技术细节（wire 协议、并发模型、会话格式、与 pi 的映射、测试策略）见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

```
include/pi/
├── ai/       Json 别名 + TransportAdapter 接口（唯一知道 HTTP/SSE 的层）
│             openai_transport：内置轻量 openai 客户端实现（流式 SSE/usage/thinking/工具调用/abort）
│             model_registry：内置模型元数据（deepseek-v4-flash / deepseek-v4-pro）+ 运行时 override
│             model_list：GET /models → 可用模型列表与默认模型来源
│             sse_parser：独立 SSE 帧解析
├── agent/    Agent（prompt/steer/abort/reset/subscribe/waitForIdle）
│             agent_loop（双队列注入时机、串/并行工具批次、钩子、terminate 语义）
│             json_schema（内置轻量 JSON Schema 校验）、subagent_tool（嵌套/事件转发/abort 链/深度轮次防护）
├── harness/  Session（JSONL version-3，线性历史）+ JsonlSessionRepo
│             compaction（切点/估算/摘要）、skills（SKILL.md）、prompt_templates
│             AgentHarness（消息持久化 + 模型/thinking 恢复）
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

测试对照 pi 的 vitest 套件：`sse_parser_test`、`transport_test`（fake SSE server + 录制 fixture）、`agent_loop_test`（脚本化 transport）、`session_test`/`compaction_test`/`skills_test`/`templates_test`、`subagent_tool_test`。

## 里程碑状态

M1 骨架+transport ✅ · M2 ai 层完整 ✅ · M3 agent 循环+工具 ✅ · M4 harness ✅ · M5 REPL 应用 ✅ · M6 子 agent+打磨 ✅

项目代码默认启用 `-Wall -Wextra -Wpedantic`；CI 使用 `-DPI_WARNINGS_AS_ERRORS=ON`。
`Result` 标记为 `[[nodiscard]]`，调用方必须检查失败结果。会话存储失败会向运行/UI 边界报告；
模型、thinking 与工具设置在持久化成功后才更新运行状态。

工具钩子的 `context` 为只读视图；后置钩子的 `content/details/isError/terminate` 使用
`std::optional` 表达字段覆盖，未设置表示保留原值。需要替换上下文时使用 `prepareNextTurn` 返回值。

cmake_minimum_required(VERSION 3.16)
project(pi_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 剪贴板图片读取（macOS）需要 Objective-C++ 编译
enable_language(OBJCXX)

# 导出 compile_commands.json，供 IDE（VS Code C/C++ 扩展）跟随真实编译配置
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# 设置所有 FetchContent 下载的根目录（放在项目源码同级的外部目录）
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/external")

# 声明 nlohmann/json（使用指定 tag + 浅克隆）
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

# 声明 openai-cpp（HTTP/SSE 传输实现，同样使用浅克隆）
FetchContent_Declare(
  openai-cpp
  GIT_REPOSITORY https://github.com/ItsJustMeChris/openai-cpp.git
  GIT_TAG        1.0.0
  GIT_SHALLOW    TRUE
)

# 声明 valijson（工具参数 JSON Schema 校验，header-only）
FetchContent_Declare(
  valijson
  GIT_REPOSITORY https://github.com/tristanpenman/valijson.git
  GIT_TAG        v1.0.3
  GIT_SHALLOW    TRUE
)

# 声明 stb（图片解码/缩放，header-only）
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# 声明 googletest（单元测试框架）
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)

# 查找系统 libcurl（openai-cpp 依赖）
find_package(CURL REQUIRED)

# 下载并配置所有依赖（注意顺序：先 MakeAvailable 被依赖的库）
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(openai-cpp)
FetchContent_MakeAvailable(valijson)
FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(googletest)

# ============ pi_core：ai + agent + harness 静态库 ============
set(PI_CORE_SOURCES
  src/ai/sse_parser.cpp
  src/ai/json_util.cpp
  src/ai/model_registry.cpp
  src/ai/openai_transport.cpp
  src/agent/agent_loop.cpp
  src/agent/agent.cpp
  src/agent/json_schema.cpp
  src/agent/subagent_tool.cpp
  src/harness/env.cpp
  src/harness/uuid.cpp
  src/harness/session.cpp
  src/harness/jsonl_repo.cpp
  src/harness/compaction.cpp
  src/harness/skills.cpp
  src/harness/prompt_templates.cpp
  src/harness/agent_harness.cpp
  src/harness/system_prompt.cpp
)

add_library(pi_core STATIC ${PI_CORE_SOURCES})
target_include_directories(pi_core PUBLIC include)
target_link_libraries(pi_core PUBLIC
  nlohmann_json::nlohmann_json
  openai::openai
  CURL::libcurl
)
target_include_directories(pi_core PUBLIC ${stb_SOURCE_DIR})
target_include_directories(pi_core PUBLIC ${valijson_SOURCE_DIR}/include)

# ============ pi_repl：终端可执行 ============
add_executable(pi_repl
  apps/repl/main.cpp
  src/app/settings.cpp
  src/app/images.cpp
  src/app/commands.cpp
  src/app/repl.cpp
  src/app/agent_session.cpp
)
target_link_libraries(pi_repl PRIVATE pi_core)

# ============ pi_tests：单元测试 ============
set(PI_TEST_SOURCES
  tests/main.cpp
  tests/ai/cost_test.cpp
  tests/ai/sse_parser_test.cpp
  tests/ai/model_registry_test.cpp
  tests/ai/transport_test.cpp
  tests/agent/pending_queue_test.cpp
  tests/agent/agent_loop_test.cpp
  tests/agent/subagent_tool_test.cpp
  tests/harness/session_test.cpp
  tests/harness/compaction_test.cpp
  tests/harness/skills_test.cpp
  tests/harness/templates_test.cpp
)

add_executable(pi_tests ${PI_TEST_SOURCES})
target_include_directories(pi_tests PRIVATE tests)
target_link_libraries(pi_tests PRIVATE pi_core gtest_main)

enable_testing()
add_test(NAME pi_tests COMMAND pi_tests)

cmake_minimum_required(VERSION 3.16)
project(pi_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只有 macOS 的剪贴板实现需要 Objective-C++；Linux 构建不应强制依赖该编译器。
if(APPLE)
  enable_language(OBJCXX)
endif()

# 导出 compile_commands.json，供 IDE（VS Code C/C++ 扩展）跟随真实编译配置
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# 每个构建目录使用独立依赖树，避免 Make/Ninja 或不同平台共享 CMake 产物。
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")

# nlohmann/json 是 header-only 库，官方提供单头文件 single_include/nlohmann/json.hpp，
# 直接 vendor 在 third_party/nlohmann_json/（见其 README），不再整仓库 FetchContent。

# 声明 openai-cpp（HTTP/SSE 传输实现，同样使用浅克隆）
FetchContent_Declare(
  openai-cpp
  GIT_REPOSITORY https://github.com/ItsJustMeChris/openai-cpp.git
  GIT_TAG        1.0.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)

# 声明 valijson（工具参数 JSON Schema 校验，header-only）
# GIT_SUBMODULES 置空：本项目只引用 valijson 的 include/ 头文件，其子模块
# （jsoncpp/nlohmann-json/JSON-Schema-Test-Suite）是 valijson 自身测试的依赖，
# 跳过可省 3 次网络克隆，也避免其中任一仓库传输失败拖垮整个 configure。
FetchContent_Declare(
  valijson
  GIT_REPOSITORY https://github.com/tristanpenman/valijson.git
  GIT_TAG        v1.0.3
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  GIT_SUBMODULES ""
)

# stb 是纯 header 库且上游无 CMake 构建文件，直接 vendor 在 third_party/stb/（见其 README）

# 声明 googletest（单元测试框架）
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)

# 查找系统 libcurl（openai-cpp 依赖）
find_package(CURL REQUIRED)

# 下载并配置所有依赖（注意顺序：先 MakeAvailable 被依赖的库）
FetchContent_MakeAvailable(openai-cpp)
FetchContent_MakeAvailable(valijson)
FetchContent_MakeAvailable(googletest)


# openai-cpp 1.0.0 的若干头文件缺失显式 include：webhooks.hpp 用 std::variant，
# utils/to_file.hpp、audio.hpp、videos.hpp 用 std::uint8_t（缺 <cstdint>）。
# libc++ 常通过传递包含掩盖该问题，GCC/libstdc++（Ubuntu CI）会直接报错。
# 对第三方 target 强制包含统一兼容头（见 cmake/openai_cpp_compat.h），
# 避免修改 FetchContent 生成的 external/ 源码。
# 注意：必须保持单个 -include 标志——CMake 对带值选项会做去重合并，
# 写两个 -include 会被拼成一个，第二个值被当作源文件导致编译失败。
if(TARGET openai-cpp AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(openai-cpp PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/openai_cpp_compat.h)
endif()

# openai-cpp 自带 external/nlohmann/json.hpp（v3.9.1，无版本 inline namespace），
# 而本项目用 vendored v3.11.3（nlohmann::json_v3_11_3）。两者是字面不同的类型，
# 公共头文件内嵌 nlohmann::json 成员又跨库边界传递，构成 ODR 隐患。
# 用 BEFORE 让 openai-cpp 编译时优先解析到本项目 vendored 的 v3.11.3，
# 使库与调用方同版本（见 third_party/nlohmann_json/README.md）。
if(TARGET openai-cpp)
  target_include_directories(openai-cpp BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)
endif()

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
  openai::openai
  CURL::libcurl
)
target_include_directories(pi_core PUBLIC ${valijson_SOURCE_DIR}/include)
# vendored stb（见 third_party/stb/README.md）
target_include_directories(pi_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
# vendored nlohmann/json（见 third_party/nlohmann_json/README.md）
target_include_directories(pi_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)

# ============ picpp：终端可执行 ============
add_executable(picpp
  apps/repl/main.cpp
  src/app/settings.cpp
  src/app/images.cpp
  src/app/memory_monitor.cpp
  src/app/commands.cpp
  src/app/repl.cpp
  src/app/agent_session.cpp
)
target_link_libraries(picpp PRIVATE pi_core)

# ============ pi_tests：单元测试 ============
set(PI_TEST_SOURCES
  tests/main.cpp
  tests/ai/cost_test.cpp
  tests/ai/json_util_test.cpp
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
  tests/app/settings_test.cpp
  tests/app/commands_test.cpp
  tests/app/memory_monitor_test.cpp
  src/app/settings.cpp
  src/app/commands.cpp
  src/app/memory_monitor.cpp
)

add_executable(pi_tests ${PI_TEST_SOURCES})
target_include_directories(pi_tests PRIVATE tests)
target_link_libraries(pi_tests PRIVATE pi_core gtest_main)

enable_testing()
add_test(NAME pi_tests COMMAND pi_tests)

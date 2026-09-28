cmake_minimum_required(VERSION 3.16)
project(pi_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PI_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
set(PI_SANITIZER "" CACHE STRING "Sanitizer: address or thread")
set_property(CACHE PI_SANITIZER PROPERTY STRINGS "" address thread)
add_library(pi_build_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pi_build_options INTERFACE -Wall -Wextra -Wpedantic)
  if(PI_WARNINGS_AS_ERRORS)
    target_compile_options(pi_build_options INTERFACE -Werror)
  endif()
  if(PI_SANITIZER STREQUAL "address")
    target_compile_options(pi_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pi_build_options INTERFACE -fsanitize=address,undefined)
  elseif(PI_SANITIZER STREQUAL "thread")
    target_compile_options(pi_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(pi_build_options INTERFACE -fsanitize=thread)
  elseif(NOT PI_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unsupported PI_SANITIZER: ${PI_SANITIZER}")
  endif()
endif()

option(BUILD_TESTING "Build google test" ON)
# 只有 macOS 的剪贴板实现需要 Objective-C++；Linux 构建不应强制依赖该编译器。
if(APPLE)
  enable_language(OBJCXX)
endif()

# 导出 compile_commands.json，供 IDE（VS Code C/C++ 扩展）跟随真实编译配置
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# 每个构建目录使用独立依赖树，避免 Make/Ninja 或不同平台共享 CMake 产物。
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")

# JSON and stb are vendored header libraries. Only tests need FetchContent.
if(BUILD_TESTING)
  # 声明 googletest（单元测试框架）
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
  )
endif()
# 查找系统 libcurl（内置 OpenAI 客户端的 HTTP 传输依赖）
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# 下载并配置测试依赖
if(BUILD_TESTING)
  FetchContent_MakeAvailable(googletest)
endif()

# ============ pi_core：ai + agent + harness 静态库 ============
set(PI_CORE_SOURCES
  src/ai/sse_parser.cpp
  src/ai/json_util.cpp
  src/ai/model_registry.cpp
  src/ai/openai_mini.cpp
  src/ai/openai_transport.cpp
  src/ai/curl_http_client.cpp
  src/agent/agent_loop.cpp
  src/agent/agent.cpp
  src/agent/json_schema.cpp
  src/agent/subagent_tool.cpp
  src/harness/env.cpp
  src/harness/posix_shell.cpp
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
  CURL::libcurl Threads::Threads
  PRIVATE pi_build_options
)
# vendored nlohmann/json（见 third_party/nlohmann_json/README.md）
target_include_directories(pi_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)

# ============ picpp：终端可执行 ============
add_executable(picpp
  apps/repl/main.cpp
  src/app/settings.cpp
  src/app/images.cpp
  src/app/memory_monitor.cpp
  src/app/commands.cpp
  src/app/repl.cpp
  src/app/repl_commands.cpp
  src/app/terminal_input.cpp
  src/app/agent_session.cpp
)
target_link_libraries(picpp PRIVATE pi_core pi_build_options)
target_include_directories(picpp SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

if(BUILD_TESTING)
  # ============ pi_tests：单元测试 ============
  set(PI_TEST_SOURCES
    tests/main.cpp
    tests/ai/cost_test.cpp
    tests/ai/json_util_test.cpp
    tests/ai/sse_parser_test.cpp
    tests/ai/model_registry_test.cpp
    tests/ai/transport_test.cpp
    tests/ai/openai_mini_test.cpp
    tests/ai/curl_http_client_test.cpp
    tests/agent/pending_queue_test.cpp
    tests/agent/agent_loop_test.cpp
    tests/agent/json_schema_test.cpp
    tests/agent/subagent_tool_test.cpp
    tests/harness/session_test.cpp
    tests/harness/posix_shell_test.cpp
    tests/harness/agent_harness_test.cpp
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
  target_include_directories(pi_tests PRIVATE tests src)
  target_link_libraries(pi_tests PRIVATE pi_core gtest_main pi_build_options)

  enable_testing()
  add_test(NAME pi_tests COMMAND pi_tests)
  set_tests_properties(pi_tests PROPERTIES TIMEOUT 120)
endif()

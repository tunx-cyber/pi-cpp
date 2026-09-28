#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>

#include <stdexcept>

#include "pi/harness/env.h"

namespace pi
{
namespace
{
int count_open_descriptors()
{
    int count = 0;
    // Test processes use low-numbered descriptors; avoid platform-specific /proc APIs.
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

TEST(PosixShellTest, CancellationClosesPipesAndReapsChild)
{
    PosixShell shell;
    const int before = count_open_descriptors();
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        ExecOptions options;
        options.abort = std::make_shared<std::atomic<bool>>(false);
        options.onStdout = [&](const std::string&) { options.abort->store(true); };
        options.timeoutSeconds = 3;
        const auto result = shell.exec("printf ready; sleep 20", options);
        ASSERT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, ExecutionErrorCode::Aborted);
    }
    EXPECT_EQ(count_open_descriptors(), before);
}

TEST(PosixShellTest, TimeoutClosesPipes)
{
    PosixShell shell;
    ExecOptions options;
    options.timeoutSeconds = 0;
    const int before = count_open_descriptors();
    const auto result = shell.exec("sleep 20", options);
    ASSERT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, ExecutionErrorCode::Timeout);
    EXPECT_EQ(count_open_descriptors(), before);
}

TEST(PosixShellTest, InvalidWorkingDirectoryDoesNotExecuteCommand)
{
    PosixShell shell;
    ExecOptions options;
    options.cwd = "/dev/null/not-a-directory";
    bool received_output = false;
    options.onStdout = [&](const std::string&) { received_output = true; };
    const auto result = shell.exec("printf should-not-run", options);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, ExecutionErrorCode::SpawnError);
    EXPECT_FALSE(received_output);
}

TEST(PosixShellTest, ThrowingOutputCallbackCleansUp)
{
    PosixShell shell;
    ExecOptions options;
    options.onStdout = [](const std::string&) { throw std::runtime_error("consumer failed"); };
    const int before = count_open_descriptors();
    const auto start = std::chrono::steady_clock::now();
    const auto result = shell.exec("printf ready; sleep 20", options);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, ExecutionErrorCode::CallbackError);
    EXPECT_EQ(result.error.message, "consumer failed");
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(count_open_descriptors(), before);
}

TEST(PosixShellTest, CapturesBothStreamsExitCodeAndEnvironment)
{
    PosixShell shell;
    ExecOptions options;
    options.env["PI_SHELL_TEST_VALUE"] = "hello";
    const auto result =
        shell.exec("printf '%s' \"$PI_SHELL_TEST_VALUE\"; printf error >&2; exit 7", options);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.stdout, "hello");
    EXPECT_EQ(result.value.stderr, "error");
    EXPECT_EQ(result.value.exitCode, 7);
}
}  // namespace
}  // namespace pi

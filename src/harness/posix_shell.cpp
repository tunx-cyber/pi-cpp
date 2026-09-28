#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include <algorithm>
#include <stdexcept>
#include <thread>

#include "pi/harness/env.h"
#include "pi/util/scope_exit.h"
#include "unique_fd.h"

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif

namespace pi
{
namespace
{
using ExecResponse = Result<ExecResult, ExecutionError>;
std::mutex spawn_mutex;

ExecResponse failure(ExecutionErrorCode code, const std::string& message)
{
    return ExecResponse::err_value({code, message});
}

struct Pipe
{
    UniqueFd read;
    UniqueFd write;

    Pipe()
    {
        int fds[2];
#if defined(__linux__)
        if (::pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error(std::strerror(errno));
#else
        if (::pipe(fds) != 0) throw std::runtime_error(std::strerror(errno));
#endif
        read.reset(fds[0]);
        write.reset(fds[1]);
        // Leave stdin/stdout/stderr available for dup2 even if the caller closed them.
        for (auto* fd : {&read, &write})
        {
            if (fd->get() >= STDERR_FILENO + 1) continue;
            const int replacement = fcntl(fd->get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (replacement < 0) throw std::runtime_error(std::strerror(errno));
            fd->reset(replacement);
        }
        // Prevent unrelated shell commands from retaining these pipes after exec.
        if (fcntl(read.get(), F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(write.get(), F_SETFD, FD_CLOEXEC) < 0)
            throw std::runtime_error(std::strerror(errno));
    }
};

void terminate_group(pid_t pid) noexcept
{
    kill(-pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    kill(-pid, SIGKILL);
}

// Only async-signal-safe operations may run in the child before exec.
[[noreturn]] void child_failed(int fd) noexcept
{
    const int error = errno;
    ssize_t written;
    do
    {
        written = ::write(fd, &error, sizeof(error));
    } while (written < 0 && errno == EINTR);
    _exit(127);
}

void make_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error(std::strerror(errno));
}

void drain(int fd, std::string& output, const std::function<void(const std::string&)>& callback,
           size_t budget = 65536)
{
    char buffer[4096];
    // Yield to cancellation/timeout checks even if a child writes continuously.
    while (budget > 0)
    {
        const ssize_t count = ::read(fd, buffer, std::min(sizeof(buffer), budget));
        if (count > 0)
        {
            budget -= static_cast<size_t>(count);
            output.append(buffer, static_cast<size_t>(count));
            if (callback) callback(std::string(buffer, static_cast<size_t>(count)));
        }
        else if (count < 0 && errno == EINTR)
        {
            continue;
        }
        else if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return;
        }
        else
        {
            throw std::runtime_error(std::strerror(errno));
        }
    }
}
}  // namespace

std::mutex PosixShell::children_mutex_;
std::set<pid_t> PosixShell::children_;

void PosixShell::register_child(pid_t pid)
{
    std::lock_guard<std::mutex> lock(children_mutex_);
    children_.insert(pid);
}

void PosixShell::kill_all_children()
{
    // Keep registrations stable until signaling finishes, avoiding stale PID snapshots.
    std::lock_guard<std::mutex> lock(children_mutex_);
    for (pid_t pid : children_) kill(-pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (pid_t pid : children_) kill(-pid, SIGKILL);
}

Result<ExecResult, ExecutionError> PosixShell::exec(const std::string& command,
                                                    const ExecOptions& options)
{
    if (options.abort && options.abort->load())
        return failure(ExecutionErrorCode::Aborted, "Command aborted");

    // Build the environment before fork; child-side allocation is unsafe in a threaded process.
    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    if (!options.env.empty())
    {
        for (char** entry = environ; *entry; ++entry) env_storage.emplace_back(*entry);
        for (const auto& [key, value] : options.env)
        {
            const std::string prefix = key + "=";
            bool replaced = false;
            for (auto& existing : env_storage)
            {
                if (existing.compare(0, prefix.size(), prefix) == 0)
                {
                    existing = prefix + value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) env_storage.push_back(prefix + value);
        }
        for (auto& entry : env_storage) envp.push_back(entry.data());
        envp.push_back(nullptr);
    }

    try
    {
        // Serialize pipe setup with our own fork calls (macOS lacks pipe2).
        std::unique_lock<std::mutex> spawn_lock(spawn_mutex);
        Pipe output_pipe;
        Pipe error_pipe;
        Pipe spawn_pipe;
        const pid_t pid = fork();
        if (pid < 0) return failure(ExecutionErrorCode::SpawnError, std::strerror(errno));
        if (pid == 0)
        {
            if (setpgid(0, 0) != 0 || dup2(output_pipe.write.get(), STDOUT_FILENO) < 0 ||
                dup2(error_pipe.write.get(), STDERR_FILENO) < 0)
                child_failed(spawn_pipe.write.get());
            if (!options.cwd.empty() && chdir(options.cwd.c_str()) != 0)
                child_failed(spawn_pipe.write.get());
            char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                                  const_cast<char*>(command.c_str()), nullptr};
            execve("/bin/sh", argv, envp.empty() ? environ : envp.data());
            child_failed(spawn_pipe.write.get());
        }

        spawn_lock.unlock();
        // Both parent and child establish the group, so an immediate abort cannot miss it.
        setpgid(pid, pid);
        bool reaped = false;
        ScopeExit cleanup(
            [&]() noexcept
            {
                std::lock_guard<std::mutex> lock(children_mutex_);
                if (!reaped)
                {
                    terminate_group(pid);
                    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
                    {
                    }
                }
                children_.erase(pid);
            });
        register_child(pid);
        output_pipe.write.reset();
        error_pipe.write.reset();
        spawn_pipe.write.reset();

        int spawn_error = 0;
        ssize_t count;
        do
        {
            count = ::read(spawn_pipe.read.get(), &spawn_error, sizeof(spawn_error));
        } while (count < 0 && errno == EINTR);
        if (count > 0)
            return failure(ExecutionErrorCode::SpawnError,
                           "Cannot start command: " + std::string(std::strerror(spawn_error)));
        if (count < 0) return failure(ExecutionErrorCode::SpawnError, std::strerror(errno));
        spawn_pipe.read.reset();

        make_nonblocking(output_pipe.read.get());
        make_nonblocking(error_pipe.read.get());
        const auto started = std::chrono::steady_clock::now();
        ExecResult result;
        int status = 0;
        try
        {
            while (!reaped)
            {
                if (options.abort && options.abort->load())
                    return failure(ExecutionErrorCode::Aborted, "Command aborted");
                if (options.timeoutSeconds && std::chrono::steady_clock::now() - started >=
                                                  std::chrono::seconds(*options.timeoutSeconds))
                    return failure(ExecutionErrorCode::Timeout, "Command timed out");

                pollfd fds[] = {{output_pipe.read.get(), POLLIN, 0},
                                {error_pipe.read.get(), POLLIN, 0}};
                if (poll(fds, 2, 50) < 0 && errno != EINTR)
                    return failure(ExecutionErrorCode::Unknown, std::strerror(errno));
                drain(output_pipe.read.get(), result.stdout, options.onStdout);
                drain(error_pipe.read.get(), result.stderr, options.onStderr);
                std::lock_guard<std::mutex> children_lock(children_mutex_);
                const pid_t waited = waitpid(pid, &status, WNOHANG);
                if (waited == pid)
                {
                    reaped = true;
                    children_.erase(pid);
                }
                else if (waited < 0 && errno != EINTR)
                {
                    // ECHILD means another owner already reaped it; do not signal a stale PID.
                    reaped = errno == ECHILD;
                    return failure(ExecutionErrorCode::Unknown, std::strerror(errno));
                }
            }
            // Drain only bytes already buffered when the shell exits. A background
            // descendant that keeps writing must not prolong the command indefinitely.
            const auto drain_remaining = [&](int fd, std::string& output, const auto& callback)
            {
                int available = 0;
                if (ioctl(fd, FIONREAD, &available) < 0)
                    throw std::runtime_error(std::strerror(errno));
                drain(fd, output, callback, static_cast<size_t>(available));
            };
            drain_remaining(output_pipe.read.get(), result.stdout, options.onStdout);
            drain_remaining(error_pipe.read.get(), result.stderr, options.onStderr);
        }
        catch (const std::exception& error)
        {
            return failure(ExecutionErrorCode::CallbackError, error.what());
        }
        catch (...)
        {
            return failure(ExecutionErrorCode::CallbackError, "Output callback failed");
        }
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                                            : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        return ExecResponse::ok_value(std::move(result));
    }
    catch (const std::exception& error)
    {
        return failure(ExecutionErrorCode::SpawnError, error.what());
    }
}
}  // namespace pi

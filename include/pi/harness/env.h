#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "pi/harness/types.h"

namespace pi
{

/** POSIX 文件系统实现。 */
class PosixFileSystem : public FileSystem
{
   public:
    explicit PosixFileSystem(std::string cwd) : cwd_(std::move(cwd)) {}

    std::string cwd() const override { return cwd_; }

    Result<std::string, FileError> absolute_path(const std::string& path) override;
    Result<std::string, FileError> join_path(const std::vector<std::string>& parts) override;
    Result<std::string, FileError> read_text_file(const std::string& path) override;
    Result<std::vector<std::string>, FileError> read_text_lines(const std::string& path,
                                                                int maxLines) override;
    Result<void, FileError> write_file(const std::string& path,
                                       const std::string& content) override;
    Result<void, FileError> append_file(const std::string& path,
                                        const std::string& content) override;
    Result<FileInfo, FileError> file_info(const std::string& path) override;
    Result<std::vector<FileInfo>, FileError> list_dir(const std::string& path) override;
    Result<std::string, FileError> canonical_path(const std::string& path) override;
    Result<bool, FileError> exists(const std::string& path) override;
    Result<void, FileError> create_dir(const std::string& path, bool recursive) override;
    Result<void, FileError> remove(const std::string& path, bool recursive) override;

   private:
    std::string resolve(const std::string& path) const;
    std::string cwd_;
};

/** POSIX shell：fork/exec /bin/sh，abort 时 kill。 */
class PosixShell : public Shell
{
   public:
    Result<ExecResult, ExecutionError> exec(const std::string& command,
                                            const ExecOptions& options) override;

    /** 强制终止所有通过本类启动的子进程（进程组）。退出前调用一次。 */
    static void kill_all_children();

   private:
    static void register_child(pid_t pid);
    static void unregister_child(pid_t pid);

    static std::mutex children_mutex_;
    static std::set<pid_t> children_;
};

}  // namespace pi

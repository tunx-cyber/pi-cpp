#include "pi/harness/env.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <fstream>
#include <sstream>

namespace pi
{

namespace
{

FileError make_file_error(FileErrorCode code, const std::string& message, const std::string& path)
{
    FileError error;
    error.code = code;
    error.message = message;
    error.path = path;
    return error;
}

bool is_dir(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string errno_message() { return std::strerror(errno); }

}  // namespace

std::string PosixFileSystem::resolve(const std::string& path) const
{
    if (!path.empty() && path.front() == '/') return path;
    return cwd_ + "/" + path;
}

Result<std::string, FileError> PosixFileSystem::absolute_path(const std::string& path)
{
    const std::string resolved = resolve(path);
    // 语法归一化（不解析符号链接）
    std::vector<std::string> segments;
    std::stringstream stream(resolved);
    std::string segment;
    while (std::getline(stream, segment, '/'))
    {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..")
        {
            if (!segments.empty()) segments.pop_back();
            continue;
        }
        segments.push_back(segment);
    }
    std::string out = "/";
    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (i > 0) out += "/";
        out += segments[i];
    }
    if (out.empty()) out = "/";
    return Result<std::string, FileError>::ok_value(out);
}

Result<std::string, FileError> PosixFileSystem::join_path(const std::vector<std::string>& parts)
{
    std::string joined;
    for (const auto& part : parts)
    {
        if (part.empty()) continue;
        if (!joined.empty() && joined.back() != '/') joined += "/";
        if (part.front() == '/' && !joined.empty() && joined.back() == '/')
        {
            joined += part.substr(1);
        }
        else
        {
            joined += part;
        }
    }
    return absolute_path(joined);
}

Result<std::string, FileError> PosixFileSystem::read_text_file(const std::string& path)
{
    const std::string resolved = resolve(path);
    std::ifstream file(resolved, std::ios::binary);
    if (!file)
        return Result<std::string, FileError>::err_value(make_file_error(
            FileErrorCode::NotFound, "Failed to read file: " + errno_message(), resolved));
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return Result<std::string, FileError>::ok_value(buffer.str());
}

Result<std::vector<std::string>, FileError> PosixFileSystem::read_text_lines(
    const std::string& path, int maxLines)
{
    const std::string resolved = resolve(path);
    std::ifstream file(resolved);
    if (!file)
        return Result<std::vector<std::string>, FileError>::err_value(make_file_error(
            FileErrorCode::NotFound, "Failed to read file: " + errno_message(), resolved));
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (maxLines > 0 && static_cast<int>(lines.size()) >= maxLines) break;
    }
    return Result<std::vector<std::string>, FileError>::ok_value(std::move(lines));
}

Result<void, FileError> PosixFileSystem::write_file(const std::string& path,
                                                    const std::string& content)
{
    const std::string resolved = resolve(path);
    const std::string dir = resolved.substr(0, resolved.find_last_of('/'));
    if (!dir.empty())
    {
        if (auto result = create_dir(dir, true); !result.ok) return result;
    }
    std::ofstream file(resolved, std::ios::binary | std::ios::trunc);
    if (!file)
        return Result<void, FileError>::err_value(make_file_error(
            FileErrorCode::Unknown, "Failed to write file: " + errno_message(), resolved));
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.flush();
    if (!file.good())
    {
        return Result<void, FileError>::err_value(make_file_error(
            FileErrorCode::Unknown, "Failed to write file: " + errno_message(), resolved));
    }
    return Result<void, FileError>::ok_value();
}

Result<void, FileError> PosixFileSystem::append_file(const std::string& path,
                                                     const std::string& content)
{
    const std::string resolved = resolve(path);
    const std::string dir = resolved.substr(0, resolved.find_last_of('/'));
    if (!dir.empty())
    {
        if (auto result = create_dir(dir, true); !result.ok) return result;
    }
    std::ofstream file(resolved, std::ios::binary | std::ios::app);
    if (!file)
        return Result<void, FileError>::err_value(make_file_error(
            FileErrorCode::Unknown, "Failed to append file: " + errno_message(), resolved));
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.flush();
    if (!file.good())
    {
        return Result<void, FileError>::err_value(make_file_error(
            FileErrorCode::Unknown, "Failed to append file: " + errno_message(), resolved));
    }
    return Result<void, FileError>::ok_value();
}

Result<FileInfo, FileError> PosixFileSystem::file_info(const std::string& path)
{
    const std::string resolved = resolve(path);
    struct stat st;
    if (lstat(resolved.c_str(), &st) != 0)
    {
        if (errno == ENOENT)
        {
            return Result<FileInfo, FileError>::err_value(
                make_file_error(FileErrorCode::NotFound, "Path does not exist", resolved));
        }
        return Result<FileInfo, FileError>::err_value(
            make_file_error(FileErrorCode::Unknown, errno_message(), resolved));
    }
    FileInfo info;
    info.path = resolved;
    info.name = resolved.substr(resolved.find_last_of('/') + 1);
    if (S_ISLNK(st.st_mode))
    {
        info.kind = FileKind::Symlink;
    }
    else if (S_ISDIR(st.st_mode))
    {
        info.kind = FileKind::Directory;
    }
    else
    {
        info.kind = FileKind::File;
    }
    info.size = static_cast<int64_t>(st.st_size);
    info.mtimeMs = static_cast<int64_t>(st.st_mtime) * 1000;
    return Result<FileInfo, FileError>::ok_value(std::move(info));
}

Result<std::vector<FileInfo>, FileError> PosixFileSystem::list_dir(const std::string& path)
{
    const std::string resolved = resolve(path);
    DIR* dir = opendir(resolved.c_str());
    if (!dir)
    {
        return Result<std::vector<FileInfo>, FileError>::err_value(make_file_error(
            FileErrorCode::NotFound, "Failed to list directory: " + errno_message(), resolved));
    }
    std::vector<FileInfo> entries;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = resolved + "/" + name;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        FileInfo info;
        info.name = name;
        info.path = full;
        if (S_ISLNK(st.st_mode))
        {
            info.kind = FileKind::Symlink;
        }
        else if (S_ISDIR(st.st_mode))
        {
            info.kind = FileKind::Directory;
        }
        else
        {
            info.kind = FileKind::File;
        }
        info.size = static_cast<int64_t>(st.st_size);
        info.mtimeMs = static_cast<int64_t>(st.st_mtime) * 1000;
        entries.push_back(std::move(info));
    }
    closedir(dir);
    return Result<std::vector<FileInfo>, FileError>::ok_value(std::move(entries));
}

Result<std::string, FileError> PosixFileSystem::canonical_path(const std::string& path)
{
    const std::string resolved = resolve(path);
    char* real = realpath(resolved.c_str(), nullptr);
    if (!real)
    {
        if (errno == ENOENT)
        {
            return Result<std::string, FileError>::err_value(
                make_file_error(FileErrorCode::NotFound, "Path does not exist", resolved));
        }
        return Result<std::string, FileError>::err_value(
            make_file_error(FileErrorCode::Unknown, errno_message(), resolved));
    }
    std::string result(real);
    free(real);
    return Result<std::string, FileError>::ok_value(std::move(result));
}

Result<bool, FileError> PosixFileSystem::exists(const std::string& path)
{
    const std::string resolved = resolve(path);
    struct stat st;
    if (lstat(resolved.c_str(), &st) == 0) return Result<bool, FileError>::ok_value(true);
    if (errno == ENOENT) return Result<bool, FileError>::ok_value(false);
    return Result<bool, FileError>::err_value(
        make_file_error(FileErrorCode::Unknown, errno_message(), resolved));
}

Result<void, FileError> PosixFileSystem::create_dir(const std::string& path, bool recursive)
{
    const std::string resolved = resolve(path);
    if (is_dir(resolved)) return Result<void, FileError>::ok_value();
    if (!recursive)
    {
        if (mkdir(resolved.c_str(), 0755) != 0)
        {
            return Result<void, FileError>::err_value(
                make_file_error(FileErrorCode::Unknown, errno_message(), resolved));
        }
        return Result<void, FileError>::ok_value();
    }
    size_t pos = 1;
    while (pos != std::string::npos)
    {
        pos = resolved.find('/', pos);
        const std::string prefix = pos == std::string::npos ? resolved : resolved.substr(0, pos);
        if (!prefix.empty() && !is_dir(prefix))
        {
            if (mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST)
            {
                return Result<void, FileError>::err_value(
                    make_file_error(FileErrorCode::Unknown, errno_message(), prefix));
            }
        }
        if (pos == std::string::npos) break;
        ++pos;
    }
    return Result<void, FileError>::ok_value();
}

Result<void, FileError> PosixFileSystem::remove(const std::string& path, bool recursive)
{
    const std::string resolved = resolve(path);
    const auto info = file_info(resolved);
    if (!info.ok) return Result<void, FileError>::err_value(info.error);
    if (info.value.kind == FileKind::Directory && recursive)
    {
        const auto entries = list_dir(resolved);
        if (!entries.ok) return Result<void, FileError>::err_value(entries.error);
        for (const auto& entry : entries.value)
        {
            auto result = remove(entry.path, true);
            if (!result.ok) return result;
        }
    }
    if (::remove(resolved.c_str()) != 0)
    {
        return Result<void, FileError>::err_value(
            make_file_error(FileErrorCode::Unknown, errno_message(), resolved));
    }
    return Result<void, FileError>::ok_value();
}

}  // namespace pi

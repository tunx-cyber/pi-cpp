#include "pi/app/commands.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <optional>
#include <sstream>

#include "pi/harness/env.h"
#include "pi/harness/prompt_templates.h"

namespace pi
{

void CommandRegistry::register_command(const std::string& name, const std::string& description,
                                       CommandHandler handler)
{
    commands_[name] = Entry{description, std::move(handler)};
}

bool CommandRegistry::run(const CommandContext& context, const std::string& input) const
{
    const size_t space = input.find_first_of(" \t");
    const std::string name =
        input.substr(1, space == std::string::npos ? std::string::npos : space - 1);
    const std::string args_str = space == std::string::npos ? "" : input.substr(space + 1);
    const auto it = commands_.find(name);
    if (it == commands_.end()) return false;
    return it->second.handler(context, parse_command_args(args_str));
}

std::string CommandRegistry::help() const
{
    std::string out = "Commands:\n";
    for (const auto& [name, entry] : commands_)
    {
        out += "  /" + name + " - " + entry.description + "\n";
    }
    return out;
}

// ---------- 编码工具 ----------

namespace
{

AgentTool make_tool(
    std::string name, std::string description, Json parameters,
    std::function<ToolResult(const Json&, const std::shared_ptr<std::atomic<bool>>&)> execute)
{
    AgentTool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.label = tool.name;
    tool.parameters = std::move(parameters);
    tool.execute = [execute = std::move(execute)](
                       const std::string&, const Json& args,
                       const std::shared_ptr<std::atomic<bool>>& signal,
                       const std::function<void(const ToolResult&)>&) -> ToolResult
    { return execute(args, signal); };
    return tool;
}

std::string read_file_text(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string join_lines(const std::vector<std::string>& lines, size_t maxLines)
{
    std::string out;
    for (size_t i = 0; i < lines.size() && i < maxLines; ++i)
    {
        out += lines[i] + "\n";
    }
    if (lines.size() > maxLines)
    {
        out += "[... " + std::to_string(lines.size() - maxLines) + " more lines truncated]";
    }
    return out;
}

std::string detect_workspace_root(const std::string& cwd)
{
    std::error_code ec;
    auto directory = std::filesystem::weakly_canonical(cwd, ec);
    if (ec || !std::filesystem::is_directory(directory, ec)) return cwd;

    // Running from build/ is common. Use the nearest project marker as the tool workspace,
    // while retaining cwd when the binary is launched outside a source checkout.
    while (true)
    {
        const bool has_git = std::filesystem::is_directory(directory / ".git", ec);
        const bool has_cmake = std::filesystem::is_regular_file(directory / "CMakeLists.txt", ec);
        if (has_git || has_cmake) return directory.string();
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return cwd;
}

// Resolve tool paths inside the project workspace and follow symlinks before checking containment.
// This permits build/../src while preventing escapes through ../ or symlinks.
std::optional<std::string> workspace_path(const std::string& cwd, const std::string& requested,
                                          bool allow_missing, const std::string& workspace_root)
{
    if (requested.empty()) return std::nullopt;
    std::error_code ec;
    const auto working_dir = std::filesystem::weakly_canonical(cwd, ec);
    if (ec) return std::nullopt;
    const auto root = std::filesystem::weakly_canonical(workspace_root, ec);
    if (ec) return std::nullopt;
    const auto input = std::filesystem::path(requested);
    const bool explicit_parent = !input.is_absolute() && !input.empty() &&
                                 (*input.begin() == std::filesystem::path(".."));
    const auto candidate = input.is_absolute()
                               ? input
                               : (explicit_parent ? working_dir : root) / input;
    auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
    {
        if (!allow_missing) return std::nullopt;
        const auto parent = std::filesystem::weakly_canonical(candidate.parent_path(), ec);
        if (ec) return std::nullopt;
        canonical = parent / candidate.filename();
    }
    const auto relative = std::filesystem::relative(canonical, root, ec);
    if (ec || (!relative.empty() &&
               (relative == std::filesystem::path("..") || relative.begin()->string() == "..")))
        return std::nullopt;
    return canonical.string();
}

}  // namespace

std::vector<AgentTool> make_coding_tools(const std::string& cwd)
{
    const std::string workspace = detect_workspace_root(cwd);
    std::vector<AgentTool> tools;

    tools.push_back(make_tool(
        "read", "Read a text file from disk. Returns the file content.",
        Json{{"type", "object"},
             {"properties", Json{{"path", Json{{"type", "string"},
                                               {"description", "Absolute or cwd-relative path"}}},
                                 {"maxLines", Json{{"type", "integer"},
                                                   {"description", "Cap on lines returned"}}}}},
             {"required", Json::array({"path"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), false, workspace);
            const std::string path = resolved.value_or("");
            ToolResult result;
            const auto info_result = [&]
            {
                struct stat st;
                if (lstat(path.c_str(), &st) != 0) return false;
                return S_ISREG(st.st_mode);
            }();
            if (!info_result)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: file not found: " + path;
                return result;
            }
            std::ifstream file(path);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(file, line))
            {
                // 长文件读取响应 abort（Ctrl+C/Esc）
                if (signal && signal->load()) break;
                lines.push_back(line);
                if (lines.size() >= 100000) break;
            }
            const int max_lines = args.contains("maxLines") ? args["maxLines"].get<int>() : 2000;
            std::string content = join_lines(lines, static_cast<size_t>(std::max(1, max_lines)));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = content.empty() ? "(empty file)" : content;
            return result;
        }));

    tools.push_back(make_tool(
        "bash", "Execute a shell command and return its output.",
        Json{{"type", "object"},
             {"properties",
              Json{{"command",
                    Json{{"type", "string"}, {"description", "Shell command to execute"}}},
                   {"cwd", Json{{"type", "string"},
                                {"description", "Working directory (defaults to session cwd)"}}}}},
             {"required", Json::array({"command"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            ToolResult result;
            PosixShell shell;
            ExecOptions options;
            const auto working_dir = workspace_path(
                cwd, args.contains("cwd") ? args["cwd"].get<std::string>() : workspace,
                false, workspace);
            if (!working_dir || !std::filesystem::is_directory(*working_dir))
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "[bash error] working directory must stay inside the workspace";
                return result;
            }
            options.cwd = *working_dir;
            options.abort = signal;
            options.timeoutSeconds = 120;
            const auto executed = shell.exec(args.value("command", ""), options);
            std::string text;
            if (!executed.ok)
            {
                text = "[bash error] " + executed.error.message;
            }
            else
            {
                text = executed.value.stdout;
                if (!executed.value.stderr.empty()) text += "\n[stderr]\n" + executed.value.stderr;
                if (executed.value.exitCode != 0)
                    text += "\n[exit code " + std::to_string(executed.value.exitCode) + "]";
            }
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no output)" : text;
            return result;
        }));

    tools.push_back(make_tool(
        "edit", "Apply a search/replace edit to a file.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}},
                   {"oldString", Json{{"type", "string"},
                                      {"description", "Text to find (must match exactly once)"}}},
                   {"newString", Json{{"type", "string"}}}}},
             {"required", Json::array({"path", "oldString", "newString"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), false, workspace);
            const std::string path = resolved.value_or("");
            ToolResult result;
            const std::string content = read_file_text(path);
            const std::string old_string = args.value("oldString", "");
            const std::string new_string = args.value("newString", "");
            size_t pos = content.find(old_string);
            if (pos == std::string::npos)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: oldString not found in " + path;
                return result;
            }
            if (content.find(old_string, pos + old_string.size()) != std::string::npos)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: oldString matches multiple times in " + path;
                return result;
            }
            std::string updated =
                content.substr(0, pos) + new_string + content.substr(pos + old_string.size());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: cannot write " + path;
                return result;
            }
            file.write(updated.data(), static_cast<std::streamsize>(updated.size()));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = "Edited " + path + " (replaced " +
                                         std::to_string(old_string.size()) + " chars with " +
                                         std::to_string(new_string.size()) + " chars)";
            return result;
        }));

    tools.push_back(make_tool(
        "write", "Create or overwrite a file with the given content.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}}, {"content", Json{{"type", "string"}}}}},
             {"required", Json::array({"path", "content"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", ""), true, workspace);
            const std::string path = resolved.value_or("");
            if (path.empty())
            {
                ToolResult result;
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: path must stay inside the workspace";
                return result;
            }
            ToolResult result;
            const std::string dir = path.substr(0, path.find_last_of('/'));
            if (!dir.empty())
            {
                std::filesystem::create_directories(dir);
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: cannot write " + path;
                return result;
            }
            const std::string content = args.value("content", "");
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text =
                "Wrote " + std::to_string(content.size()) + " bytes to " + path;
            return result;
        }));

    tools.push_back(make_tool(
        "grep", "Search for a pattern in files, returning matching lines.",
        Json{{"type", "object"},
             {"properties", Json{{"pattern", Json{{"type", "string"}}},
                                 {"path", Json{{"type", "string"},
                                               {"description", "File or directory to search"}}},
                                 {"maxResults", Json{{"type", "integer"}}}}},
             {"required", Json::array({"pattern"})}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", workspace), false, workspace);
            const std::string path = resolved.value_or("");
            const std::string pattern = args.value("pattern", "");
            const int max_results =
                args.contains("maxResults") ? args["maxResults"].get<int>() : 50;
            ToolResult result;
            std::vector<std::string> matches;
            std::function<void(const std::string&)> search = [&](const std::string& dir)
            {
                DIR* d = opendir(dir.c_str());
                if (!d) return;
                struct dirent* entry;
                while ((entry = readdir(d)) != nullptr &&
                       static_cast<int>(matches.size()) < max_results)
                {
                    // 目录遍历响应 abort（Ctrl+C/Esc）：返回已收集的部分结果
                    if (signal && signal->load()) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == ".git" || name == "build" ||
                        name == "node_modules")
                        continue;
                    const std::string full = dir + "/" + name;
                    struct stat st;
                    if (lstat(full.c_str(), &st) != 0) continue;
                    if (S_ISDIR(st.st_mode))
                    {
                        search(full);
                        continue;
                    }
                    if (!S_ISREG(st.st_mode)) continue;
                    std::ifstream file(full);
                    std::string line;
                    int line_number = 0;
                    while (std::getline(file, line) &&
                           static_cast<int>(matches.size()) < max_results)
                    {
                        if (signal && signal->load()) break;
                        ++line_number;
                        if (line.find(pattern) != std::string::npos)
                        {
                            matches.push_back(full + ":" + std::to_string(line_number) + ": " +
                                              line);
                        }
                    }
                }
                closedir(d);
            };
            struct stat st;
            if (lstat(path.c_str(), &st) != 0)
            {
                result.content.push_back(ContentBlock{});
                result.content.back().type = BlockType::Text;
                result.content.back().text = "Error: path not found: " + path;
                return result;
            }
            if (S_ISDIR(st.st_mode))
            {
                search(path);
            }
            else
            {
                std::ifstream file(path);
                std::string line;
                int line_number = 0;
                while (std::getline(file, line) && static_cast<int>(matches.size()) < max_results)
                {
                    ++line_number;
                    if (line.find(pattern) != std::string::npos)
                    {
                        matches.push_back(path + ":" + std::to_string(line_number) + ": " + line);
                    }
                }
            }
            std::string text = join_lines(matches, static_cast<size_t>(max_results));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no matches)" : text;
            return result;
        }));

    tools.push_back(make_tool(
        "find", "List files in a directory tree.",
        Json{{"type", "object"},
             {"properties",
              Json{{"path", Json{{"type", "string"}}}, {"maxResults", Json{{"type", "integer"}}}}},
             {"required", Json::array()}},
        [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>& signal) -> ToolResult
        {
            const auto resolved = workspace_path(cwd, args.value("path", workspace), false, workspace);
            const std::string path = resolved.value_or("");
            const int max_results =
                args.contains("maxResults") ? args["maxResults"].get<int>() : 100;
            ToolResult result;
            std::vector<std::string> files;
            std::function<void(const std::string&, int)> walk =
                [&](const std::string& dir, int depth)
            {
                if (depth > 6 || static_cast<int>(files.size()) >= max_results) return;
                DIR* d = opendir(dir.c_str());
                if (!d) return;
                struct dirent* entry;
                while ((entry = readdir(d)) != nullptr &&
                       static_cast<int>(files.size()) < max_results)
                {
                    // 目录遍历响应 abort（Ctrl+C/Esc）
                    if (signal && signal->load()) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == ".git" || name == "build" ||
                        name == "node_modules")
                        continue;
                    const std::string full = dir + "/" + name;
                    struct stat st;
                    if (lstat(full.c_str(), &st) != 0) continue;
                    files.push_back(full);
                    if (S_ISDIR(st.st_mode)) walk(full, depth + 1);
                }
                closedir(d);
            };
            walk(path, 0);
            std::string text = join_lines(files, static_cast<size_t>(max_results));
            result.content.push_back(ContentBlock{});
            result.content.back().type = BlockType::Text;
            result.content.back().text = text.empty() ? "(no files)" : text;
            return result;
        }));

    tools.push_back(
        make_tool("ls", "List directory contents.",
                  Json{{"type", "object"},
                       {"properties", Json{{"path", Json{{"type", "string"}}}}},
                       {"required", Json::array()}},
                  [cwd, workspace](const Json& args, const std::shared_ptr<std::atomic<bool>>&) -> ToolResult
                  {
                      const auto resolved = workspace_path(cwd, args.value("path", workspace), false,
                                                           workspace);
                      const std::string path = resolved.value_or("");
                      ToolResult result;
                      DIR* d = opendir(path.c_str());
                      if (!d)
                      {
                          result.content.push_back(ContentBlock{});
                          result.content.back().type = BlockType::Text;
                          result.content.back().text = "Error: cannot list " + path;
                          return result;
                      }
                      std::vector<std::string> names;
                      struct dirent* entry;
                      while ((entry = readdir(d)) != nullptr)
                      {
                          const std::string name = entry->d_name;
                          if (name == "." || name == "..") continue;
                          names.push_back(name);
                      }
                      closedir(d);
                      std::sort(names.begin(), names.end());
                      std::string text = join_lines(names, names.size());
                      result.content.push_back(ContentBlock{});
                      result.content.back().type = BlockType::Text;
                      result.content.back().text = text.empty() ? "(empty directory)" : text;
                      return result;
                  }));

    return tools;
}

}  // namespace pi

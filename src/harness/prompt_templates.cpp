#include "pi/harness/prompt_templates.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace pi
{

namespace
{

std::string basename_without_md(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 3 && (name.compare(name.size() - 3, 3, ".md") == 0 ||
                            name.compare(name.size() - 3, 3, ".MD") == 0))
    {
        name = name.substr(0, name.size() - 3);
    }
    return name;
}

std::map<std::string, std::string> parse_frontmatter(const std::string& content,
                                                     std::string& bodyOut)
{
    std::map<std::string, std::string> values;
    std::string normalized = content;
    std::string::size_type pos = 0;
    while ((pos = normalized.find("\r\n", pos)) != std::string::npos)
        normalized.replace(pos, 2, "\n");
    if (normalized.rfind("---", 0) != 0)
    {
        bodyOut = normalized;
        return values;
    }
    const size_t end_index = normalized.find("\n---", 3);
    if (end_index == std::string::npos)
    {
        bodyOut = normalized;
        return values;
    }
    std::string yaml_section = normalized.substr(4, end_index - 4);
    bodyOut = normalized.substr(end_index + 4);
    const size_t first = bodyOut.find_first_not_of(" \t\r\n");
    const size_t last = bodyOut.find_last_not_of(" \t\r\n");
    bodyOut = (first == std::string::npos) ? "" : bodyOut.substr(first, last - first + 1);

    std::istringstream stream(yaml_section);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key.rfind("  ", 0) == 0) continue;
        std::string value = line.substr(colon + 1);
        const size_t v_first = value.find_first_not_of(" \t");
        value = v_first == std::string::npos ? "" : value.substr(v_first);
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }
        values[key] = value;
    }
    return values;
}

}  // namespace

TemplateLoadResult load_prompt_templates(FileSystem& fs, const std::vector<std::string>& paths)
{
    TemplateLoadResult result;
    for (const auto& path : paths)
    {
        const auto info = fs.file_info(path);
        if (!info.ok)
        {
            if (info.error.code != FileErrorCode::NotFound)
            {
                result.diagnostics.push_back({"file_info_failed", info.error.message, path});
            }
            continue;
        }
        if (info.value.kind != FileKind::Directory && info.value.kind != FileKind::File) continue;

        std::vector<std::string> files_to_load;
        if (info.value.kind == FileKind::Directory)
        {
            const auto entries = fs.list_dir(info.value.path);
            if (!entries.ok)
            {
                result.diagnostics.push_back(
                    {"list_failed", entries.error.message, info.value.path});
                continue;
            }
            std::vector<FileInfo> sorted = entries.value;
            std::sort(sorted.begin(), sorted.end(),
                      [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
            for (const auto& entry : sorted)
            {
                if (entry.kind == FileKind::File && entry.name.size() > 3 &&
                    entry.name.compare(entry.name.size() - 3, 3, ".md") == 0)
                {
                    files_to_load.push_back(entry.path);
                }
            }
        }
        else
        {
            files_to_load.push_back(info.value.path);
        }

        for (const auto& file_path : files_to_load)
        {
            const auto content = fs.read_text_file(file_path);
            if (!content.ok)
            {
                result.diagnostics.push_back({"read_failed", content.error.message, file_path});
                continue;
            }
            std::string body;
            std::map<std::string, std::string> frontmatter;
            try
            {
                frontmatter = parse_frontmatter(content.value, body);
            }
            catch (const std::exception& e)
            {
                result.diagnostics.push_back({"parse_failed", e.what(), file_path});
                continue;
            }
            std::string description =
                frontmatter.count("description") ? frontmatter["description"] : "";
            if (description.empty())
            {
                const size_t first_line_end = body.find('\n');
                const std::string first_line = body.substr(0, first_line_end);
                if (!first_line.empty())
                {
                    description = first_line.substr(0, 60);
                    if (first_line.size() > 60) description += "...";
                }
            }
            PromptTemplate template_;
            template_.name = basename_without_md(file_path);
            template_.description = description;
            template_.content = body;
            result.promptTemplates.push_back(std::move(template_));
        }
    }
    return result;
}

std::vector<std::string> parse_command_args(const std::string& argsString)
{
    std::vector<std::string> args;
    std::string current;
    char in_quote = 0;
    for (size_t i = 0; i < argsString.size(); ++i)
    {
        const char c = argsString[i];
        if (in_quote)
        {
            if (c == in_quote)
            {
                in_quote = 0;
            }
            else
            {
                current += c;
            }
        }
        else if (c == '"' || c == '\'')
        {
            in_quote = c;
        }
        else if (c == ' ' || c == '\t')
        {
            if (!current.empty())
            {
                args.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += c;
        }
    }
    if (!current.empty()) args.push_back(std::move(current));
    return args;
}

std::string substitute_args(const std::string& content, const std::vector<std::string>& args)
{
    std::string result = content;
    // $N（越界替换为空，镜像 TS 的 args[n-1] ?? ""）
    for (size_t i = 0; i < 20; ++i)
    {
        const std::string pattern = "$" + std::to_string(i + 1);
        const std::string replacement = i < args.size() ? args[i] : "";
        size_t pos = 0;
        while ((pos = result.find(pattern, pos)) != std::string::npos)
        {
            // 避免匹配 $10 中的 $1
            if (pos + pattern.size() < result.size() &&
                std::isdigit(static_cast<unsigned char>(result[pos + pattern.size()])))
            {
                pos += pattern.size();
                continue;
            }
            result.replace(pos, pattern.size(), replacement);
            pos += replacement.size();
        }
    }
    // ${@:N} / ${@:N:L}
    for (size_t n = 1; n <= args.size() + 1; ++n)
    {
        const std::string pattern = "${@:" + std::to_string(n) + "}";
        size_t pos = 0;
        while ((pos = result.find(pattern, pos)) != std::string::npos)
        {
            std::string joined;
            for (size_t i = n - 1; i < args.size(); ++i)
            {
                if (!joined.empty()) joined += " ";
                joined += args[i];
            }
            result.replace(pos, pattern.size(), joined);
            pos += joined.size();
        }
        for (size_t len = 1; len <= args.size(); ++len)
        {
            const std::string slice_pattern =
                "${@:" + std::to_string(n) + ":" + std::to_string(len) + "}";
            pos = 0;
            while ((pos = result.find(slice_pattern, pos)) != std::string::npos)
            {
                std::string joined;
                const size_t start = n - 1;
                for (size_t i = start; i < start + len && i < args.size(); ++i)
                {
                    if (!joined.empty()) joined += " ";
                    joined += args[i];
                }
                result.replace(pos, slice_pattern.size(), joined);
                pos += joined.size();
            }
        }
    }
    std::string all_args;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0) all_args += " ";
        all_args += args[i];
    }
    size_t pos = 0;
    while ((pos = result.find("$ARGUMENTS", pos)) != std::string::npos)
    {
        result.replace(pos, 10, all_args);
        pos += all_args.size();
    }
    pos = 0;
    while ((pos = result.find("$@", pos)) != std::string::npos)
    {
        result.replace(pos, 2, all_args);
        pos += all_args.size();
    }
    return result;
}

std::string format_prompt_template_invocation(const PromptTemplate& template_,
                                              const std::vector<std::string>& args)
{
    return substitute_args(template_.content, args);
}

}  // namespace pi

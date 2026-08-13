#include "pi/harness/skills.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace pi
{

namespace
{

constexpr size_t kMaxNameLength = 64;
constexpr size_t kMaxDescriptionLength = 1024;

/** 简化 YAML frontmatter 解析：key: value / key: "quoted" / key: true|false。 */
std::map<std::string, std::string> parse_frontmatter(const std::string& content,
                                                     std::string& bodyOut)
{
    std::map<std::string, std::string> values;
    std::string normalized = content;
    // \r\n → \n
    std::string::size_type pos = 0;
    while ((pos = normalized.find("\r\n", pos)) != std::string::npos)
    {
        normalized.replace(pos, 2, "\n");
    }
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
    // 去首尾空白
    size_t first = bodyOut.find_first_not_of(" \t\r\n");
    size_t last = bodyOut.find_last_not_of(" \t\r\n");
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
        if (key.rfind("  ", 0) == 0) continue;  // 缩进的嵌套字段跳过
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

std::string basename(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool valid_skill_name(const std::string& name)
{
    if (name.empty()) return false;
    for (char c : name)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return !name.empty() && name.front() != '-' && name.back() != '-' &&
           name.find("--") == std::string::npos;
}

SkillLoadResult load_skill_from_file(FileSystem& fs, const std::string& filePath)
{
    SkillLoadResult result;
    const auto content = fs.read_text_file(filePath);
    if (!content.ok)
    {
        result.diagnostics.push_back({"read_failed", content.error.message, filePath});
        return result;
    }
    std::string body;
    std::map<std::string, std::string> frontmatter;
    try
    {
        frontmatter = parse_frontmatter(content.value, body);
    }
    catch (const std::exception& e)
    {
        result.diagnostics.push_back({"parse_failed", e.what(), filePath});
        return result;
    }

    const std::string skill_dir = filePath.substr(0, filePath.find_last_of('/'));
    const std::string parent_dir_name = basename(skill_dir);
    std::string description = frontmatter.count("description") ? frontmatter["description"] : "";
    std::string name = frontmatter.count("name") ? frontmatter["name"] : parent_dir_name;

    if (name != parent_dir_name)
    {
        result.diagnostics.push_back(
            {"invalid_metadata",
             "name \"" + name + "\" does not match parent directory \"" + parent_dir_name + "\"",
             filePath});
    }
    if (name.size() > kMaxNameLength)
    {
        result.diagnostics.push_back(
            {"invalid_metadata", "name exceeds " + std::to_string(kMaxNameLength) + " characters",
             filePath});
    }
    if (!valid_skill_name(name))
    {
        result.diagnostics.push_back(
            {"invalid_metadata",
             "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)",
             filePath});
    }
    if (description.empty())
    {
        result.diagnostics.push_back({"invalid_metadata", "description is required", filePath});
    }
    else if (description.size() > kMaxDescriptionLength)
    {
        result.diagnostics.push_back(
            {"invalid_metadata",
             "description exceeds " + std::to_string(kMaxDescriptionLength) + " characters",
             filePath});
    }

    if (description.empty()) return result;  // 无 description 的 skill 丢弃

    Skill skill;
    skill.name = name;
    skill.description = description;
    skill.content = body;
    skill.filePath = filePath;
    skill.disableModelInvocation = frontmatter.count("disable-model-invocation") &&
                                   frontmatter["disable-model-invocation"] == "true";
    result.skills.push_back(std::move(skill));
    return result;
}

void load_skills_from_dir(FileSystem& fs, const std::string& dir, bool includeRootFiles,
                          SkillLoadResult& result)
{
    const auto entries = fs.list_dir(dir);
    if (!entries.ok)
    {
        if (entries.error.code != FileErrorCode::NotFound)
        {
            result.diagnostics.push_back({"list_failed", entries.error.message, dir});
        }
        return;
    }

    // 优先 SKILL.md
    for (const auto& entry : entries.value)
    {
        if (entry.name == "SKILL.md" && entry.kind == FileKind::File)
        {
            const auto loaded = load_skill_from_file(fs, entry.path);
            result.skills.insert(result.skills.end(), loaded.skills.begin(), loaded.skills.end());
            result.diagnostics.insert(result.diagnostics.end(), loaded.diagnostics.begin(),
                                      loaded.diagnostics.end());
            return;
        }
    }

    std::vector<FileInfo> sorted = entries.value;
    std::sort(sorted.begin(), sorted.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
    for (const auto& entry : sorted)
    {
        if (!entry.name.empty() && entry.name.front() == '.') continue;  // 含 .git
        if (entry.name == "node_modules") continue;
        if (entry.name == "build") continue;
        if (entry.kind == FileKind::Directory)
        {
            load_skills_from_dir(fs, entry.path, false, result);
        }
        else if (entry.kind == FileKind::File && includeRootFiles && entry.name.size() > 3 &&
                 entry.name.compare(entry.name.size() - 3, 3, ".md") == 0)
        {
            const auto loaded = load_skill_from_file(fs, entry.path);
            result.skills.insert(result.skills.end(), loaded.skills.begin(), loaded.skills.end());
            result.diagnostics.insert(result.diagnostics.end(), loaded.diagnostics.begin(),
                                      loaded.diagnostics.end());
        }
    }
}

std::string escape_xml(const std::string& value)
{
    std::string out;
    for (char c : value)
    {
        switch (c)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

}  // namespace

SkillLoadResult load_skills(FileSystem& fs, const std::vector<std::string>& dirs)
{
    SkillLoadResult result;
    for (const auto& dir : dirs)
    {
        const auto info = fs.file_info(dir);
        if (!info.ok)
        {
            if (info.error.code != FileErrorCode::NotFound)
            {
                result.diagnostics.push_back({"file_info_failed", info.error.message, dir});
            }
            continue;
        }
        if (info.value.kind != FileKind::Directory) continue;
        load_skills_from_dir(fs, info.value.path, true, result);
    }
    return result;
}

std::string format_skills_for_system_prompt(const std::vector<Skill>& skills)
{
    std::vector<const Skill*> visible;
    for (const auto& skill : skills)
    {
        if (!skill.disableModelInvocation) visible.push_back(&skill);
    }
    if (visible.empty()) return "";

    std::string out =
        "The following skills provide specialized instructions for specific tasks.\n"
        "Read the full skill file when the task matches its description.\n"
        "When a skill file references a relative path, resolve it against the skill directory "
        "(parent of SKILL.md / "
        "dirname of the path) and use that absolute path in tool commands.\n\n"
        "<available_skills>\n";
    for (const auto* skill : visible)
    {
        out += "  <skill>\n";
        out += "    <name>" + escape_xml(skill->name) + "</name>\n";
        out += "    <description>" + escape_xml(skill->description) + "</description>\n";
        out += "    <location>" + escape_xml(skill->filePath) + "</location>\n";
        out += "  </skill>\n";
    }
    out += "</available_skills>";
    return out;
}

std::string format_skill_invocation(const Skill& skill, const std::string& additionalInstructions)
{
    const std::string dir = skill.filePath.substr(0, skill.filePath.find_last_of('/'));
    std::string block = "<skill name=\"" + skill.name + "\" location=\"" + skill.filePath +
                        "\">\nReferences are relative to " + dir + ".\n\n" + skill.content +
                        "\n</skill>";
    return additionalInstructions.empty() ? block : block + "\n\n" + additionalInstructions;
}

}  // namespace pi

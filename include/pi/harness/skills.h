#pragma once

#include <string>
#include <vector>

#include "pi/harness/types.h"

namespace pi
{

struct SkillDiagnostic
{
    std::string code;
    std::string message;
    std::string path;
};

struct SkillLoadResult
{
    std::vector<Skill> skills;
    std::vector<SkillDiagnostic> diagnostics;
};

/** 从目录树加载 SKILL.md（递归）与根目录直接 .md 文件。 */
SkillLoadResult load_skills(FileSystem& fs, const std::vector<std::string>& dirs);

/** <available_skills> XML 块（镜像 formatSkillsForSystemPrompt）。 */
std::string format_skills_for_system_prompt(const std::vector<Skill>& skills);

/** skill 调用提示（镜像 formatSkillInvocation）。 */
std::string format_skill_invocation(const Skill& skill,
                                    const std::string& additionalInstructions = "");

}  // namespace pi

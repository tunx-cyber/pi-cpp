#include "pi/harness/system_prompt.h"

#include "pi/harness/skills.h"

namespace pi
{

std::string build_system_prompt(const std::string& basePrompt, const std::vector<Skill>& skills,
                                const std::string& extraContext)
{
    std::string prompt = basePrompt;
    if (!prompt.empty() && prompt.back() != '\n') prompt += "\n";
    const std::string skills_block = format_skills_for_system_prompt(skills);
    if (!skills_block.empty())
    {
        prompt += "\n" + skills_block;
    }
    if (!extraContext.empty())
    {
        prompt += "\n\n" + extraContext;
    }
    return prompt;
}

}  // namespace pi

#pragma once

#include <string>
#include <vector>

#include "pi/harness/types.h"

namespace pi
{

struct PromptTemplateDiagnostic
{
    std::string code;
    std::string message;
    std::string path;
};

struct TemplateLoadResult
{
    std::vector<PromptTemplate> promptTemplates;
    std::vector<PromptTemplateDiagnostic> diagnostics;
};

/** 从目录（直接子 .md）或文件加载 prompt 模板。 */
TemplateLoadResult load_prompt_templates(FileSystem& fs, const std::vector<std::string>& paths);

/** 解析 shell 风格参数（单/双引号；镜像 parseCommandArgs）。 */
std::vector<std::string> parse_command_args(const std::string& argsString);

/** 替换模板占位符 $1/$@/$ARGUMENTS/${@:N}/${@:N:L}（镜像 substituteArgs）。 */
std::string substitute_args(const std::string& content, const std::vector<std::string>& args);

/** 模板调用（镜像 formatPromptTemplateInvocation）。 */
std::string format_prompt_template_invocation(const PromptTemplate& template_,
                                              const std::vector<std::string>& args);

}  // namespace pi

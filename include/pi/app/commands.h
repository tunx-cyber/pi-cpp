#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pi/agent/types.h"
#include "pi/app/agent_session.h"

namespace pi
{

class Repl;

/** slash 命令上下文。 */
struct CommandContext
{
    AgentSession& session;
    Repl& repl;
};

using CommandHandler =
    std::function<bool(const CommandContext&, const std::vector<std::string>& args)>;

/** 命令注册表。 */
class CommandRegistry
{
   public:
    void register_command(const std::string& name, const std::string& description,
                          CommandHandler handler);
    bool run(const CommandContext& context, const std::string& input) const;
    std::string help() const;

   private:
    struct Entry
    {
        std::string description;
        CommandHandler handler;
    };
    std::map<std::string, Entry> commands_;
};

/** 编码工具（read/bash/edit/write/grep/find/ls），供 agent 使用。 */
std::vector<AgentTool> make_coding_tools(const std::string& cwd);

}  // namespace pi

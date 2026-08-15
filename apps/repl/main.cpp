// pi-cpp 终端入口：REPL（M5 完整实现）。

#include <unistd.h>

#include <cstdlib>

#include <iostream>
#include <string>
#include <vector>

#include "pi/app/agent_session.h"
#include "pi/app/commands.h"
#include "pi/app/images.h"
#include "pi/app/repl.h"
#include "pi/app/settings.h"

int main(int argc, char** argv)
{
    std::string cwd;
    char buf[4096];
    if (getcwd(buf, sizeof(buf)))
    {
        cwd = buf;
    }

    // 解析参数：--image <path> / --resume [id] / 位置参数 prompt
    std::string prompt;
    std::vector<std::string> image_paths;
    std::string resume_id;  // 空 = 恢复最近会话
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc)
        {
            image_paths.push_back(argv[++i]);
        }
        else if (arg == "--resume")
        {
            // --resume [id]：下一个非选项参数作为会话 id（支持前缀）；
            // 不带 id 时等价于默认（恢复最近会话）。
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                resume_id = argv[++i];
            }
        }
        else if (prompt.empty())
        {
            prompt = arg;
        }
    }

    const bool piped_stdin = !isatty(STDIN_FILENO);
    // 一次性对话：管道输入、位置 prompt、或 --image（无 prompt 时读 stdin）。
    // 注意：--resume 单独出现（无 prompt/图片/管道）时进入交互式 REPL。
    const bool one_shot = piped_stdin || !prompt.empty() || !image_paths.empty();

    if (one_shot)
    {
        if (prompt.empty() && piped_stdin)
        {
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (!prompt.empty()) prompt += "\n";
                prompt += line;
            }
        }
        if (prompt.empty())
        {
            // 交互式 TTY 只给了 --image 而无 prompt：无内容可处理，直接退出
            std::cerr << "no prompt provided" << std::endl;
            return 1;
        }

        pi::Settings settings = pi::Settings::load();
        pi::AgentSession session(settings, cwd);
        if (!resume_id.empty())
        {
            std::string error;
            if (!session.resume_by_id(resume_id, &error))
            {
                std::cerr << "恢复会话失败：" << error << std::endl;
                return 1;
            }
        }
        else
        {
            session.resume();
        }
        session.set_tools(pi::make_coding_tools(cwd, settings.apiKey));

        // 流式输出到 stdout，结束打印 usage/计费
        session.subscribe(
            [](const pi::AgentEvent& event, const std::shared_ptr<std::atomic<bool>>&)
            {
                if (event.type == pi::AgentEvent::Type::MessageUpdate &&
                    event.assistantMessageEvent)
                {
                    const auto& stream_event = *event.assistantMessageEvent;
                    if (stream_event.type == pi::StreamEvent::Type::TextDelta ||
                        stream_event.type == pi::StreamEvent::Type::ThinkingDelta)
                    {
                        std::cout << stream_event.delta << std::flush;
                    }
                }
                if (event.type == pi::AgentEvent::Type::MessageEnd &&
                    event.message.role == pi::Role::Assistant)
                {
                    const auto& usage = event.message.usage;
                    std::cout << std::endl
                              << "[usage] in=" << usage.input << " out=" << usage.output
                              << " cacheRead=" << usage.cacheRead << " cost=¥" << usage.cost.total
                              << " stop=" << pi::to_string(event.message.stopReason);
                    if (!event.message.errorMessage.empty())
                    {
                        std::cout << " error=" << event.message.errorMessage;
                    }
                    std::cout << std::endl;
                }
            });
        std::vector<pi::ContentBlock> image_blocks;
        for (const auto& path : image_paths)
        {
            const auto block = pi::load_image_as_block(path);
            if (block)
            {
                image_blocks.push_back(*block);
            }
            else
            {
                std::cerr << "无法加载图片：" << path << std::endl;
            }
        }
        session.prompt(prompt, image_blocks);
        // 管道模式退出前清理 bash 工具遗留的子进程（进程组）
        pi::PosixShell::kill_all_children();
        return 0;
    }

    // 交互式 REPL
    pi::Settings settings = pi::Settings::load();
    pi::AgentSession session(settings, cwd);
    if (!resume_id.empty())
    {
        std::string error;
        if (!session.resume_by_id(resume_id, &error))
        {
            std::cerr << "恢复会话失败：" << error << std::endl;
            return 1;
        }
    }
    else
    {
        session.resume();
    }
    pi::Repl repl(session, cwd);
    return repl.run();
}

// pi-cpp 终端入口：REPL（M5 完整实现）。

#include <unistd.h>

#include <cstdlib>

#include <iostream>
#include <string>

#include "pi/app/agent_session.h"
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

    // 管道模式：`echo "hello" | pi_repl` 或 `pi_repl "prompt" [--image path]`（一次性对话）
    if (!isatty(STDIN_FILENO) || argc > 1)
    {
        std::string prompt;
        std::vector<std::string> image_paths;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--image" && i + 1 < argc)
            {
                image_paths.push_back(argv[++i]);
            }
            else if (prompt.empty())
            {
                prompt = argv[i];
            }
        }
        if (prompt.empty() && isatty(STDIN_FILENO))
        {
            // 交互式 TTY 但给了 --image：忽略
        }
        if (prompt.empty())
        {
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (!prompt.empty()) prompt += "\n";
                prompt += line;
            }
        }
        if (prompt.empty()) return 0;

        pi::Settings settings = pi::Settings::load();
        pi::AgentSession session(settings, cwd);
        session.resume();
        session.set_tools(pi::make_coding_tools(cwd));

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
                              << " cacheRead=" << usage.cacheRead << " cost=$" << usage.cost.total
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
        return 0;
    }

    pi::Settings settings = pi::Settings::load();
    pi::AgentSession session(settings, cwd);
    session.resume();
    pi::Repl repl(session, cwd);
    return repl.run();
}

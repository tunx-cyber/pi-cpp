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
#include "pi/app/theme.h"
#include "pi/app/usage_format.h"

int run_app(int argc, char** argv)
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
    bool resume_requested = false;  // --resume：恢复会话（不带 id 时恢复最近一个）
    std::string resume_id;
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
            // 不带 id 时恢复最近会话。默认（无 --resume）总是开新会话。
            resume_requested = true;
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

    /** 默认新会话；--resume 才恢复。返回 false 表示恢复失败。 */
    auto open_session = [&](pi::AgentSession& session) -> bool
    {
        if (!resume_requested)
        {
            session.new_session();
            return true;
        }
        if (resume_id.empty())
        {
            session.resume();
            return true;
        }
        std::string error;
        if (!session.resume_by_id(resume_id, &error))
        {
            std::cerr << "恢复会话失败：" << error << std::endl;
            return false;
        }
        return true;
    };

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
        if (!open_session(session)) return 1;
        // 未显式配置模型时，默认模型由端点 GET /models 返回的第一个模型确定。
        std::string model_error;
        if (!session.ensure_model(&model_error))
        {
            std::cerr << "无法确定模型：" << model_error << std::endl;
            return 1;
        }
        session.set_tools(pi::make_coding_tools(cwd, settings.apiKey, settings.webSearch));

        // 流式输出到 stdout，结束打印 usage
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
                              << "[usage] " << pi::format_token_usage(usage)
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
    if (!open_session(session)) return 1;
    // 无默认模型：没配置时向端点要模型列表，取第一个。失败不阻止进入 REPL，
    // 用户可用 /model 重新拉取（或 /model <id> 直接指定）。
    {
        std::string model_error;
        if (session.ensure_model(&model_error))
        {
            std::cout << "模型：" << session.model().id << std::endl;
        }
        else
        {
            std::cerr << "无法获取默认模型：" << model_error << std::endl;
            std::cerr << "（进入 REPL 后用 /model 选择模型，或设置 PI_MODEL）" << std::endl;
        }
    }
    // 交互模式与一次性模式一致：启动即装配编码工具（read/bash/edit/write/grep/find/ls/
    // web_fetch/web_search）。此前只有 /tools 才装，用户不敲命令就无法读写文件。
    session.set_tools(pi::make_coding_tools(cwd, settings.apiKey, settings.webSearch));
    // 主题：settings.json 的 theme 键（默认 default）；NO_COLOR 环境变量强制 mono。
    const char* no_color = std::getenv("NO_COLOR");
    const bool mono = no_color != nullptr && *no_color != '\0';
    const pi::Theme theme = pi::theme_by_name(mono ? "mono" : settings.theme);
    pi::Repl repl(session, cwd, theme, resume_requested);
    return repl.run();
}

int main(int argc, char** argv)
{
    try
    {
        return run_app(argc, argv);
    }
    catch (const std::exception& error)
    {
        pi::PosixShell::kill_all_children();
        std::cerr << "[error] " << error.what() << std::endl;
        return 1;
    }
}

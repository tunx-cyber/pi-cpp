#include "pi/app/commands.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace pi
{
namespace
{

ToolResult run_tool(const AgentTool& tool, const Json& args,
                    const std::shared_ptr<std::atomic<bool>>& signal)
{
    return tool.execute("test-call", args, signal, [](const ToolResult&) {});
}

ToolResult run_tool(const AgentTool& tool, const Json& args)
{
    return run_tool(tool, args, std::make_shared<std::atomic<bool>>(false));
}

const AgentTool& web_fetch_tool()
{
    static const std::vector<AgentTool> tools = make_coding_tools(".");
    static const auto it = std::find_if(tools.begin(), tools.end(),
                                        [](const AgentTool& tool)
                                        { return tool.name == "web_fetch"; });
    return *it;
}

/** 极简本地 HTTP/1.1 服务器：绑定 127.0.0.1 随机端口，串行处理请求。
 *  handler(path) 返回完整响应字符串；返回空串则挂起不响应（用于超时/中断场景）。
 *  仅供测试 web_fetch 的真实 libcurl 路径，不依赖外网。 */
class LocalHttpServer
{
public:
    explicit LocalHttpServer(std::function<std::string(const std::string&)> handler)
        : handler_(std::move(handler))
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("LocalHttpServer: socket failed");
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0)
        {
            ::close(listen_fd_);
            throw std::runtime_error("LocalHttpServer: bind/listen failed");
        }
        socklen_t addr_len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~LocalHttpServer()
    {
        stop_ = true;
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    int port() const { return port_; }

    std::string url(const std::string& path) const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    void serve()
    {
        while (!stop_.load())
        {
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) break;
            char buf[4096];
            const ssize_t n = ::read(client, buf, sizeof(buf) - 1);
            std::string path;
            if (n > 0)
            {
                buf[n] = '\0';
                const std::string request(buf);
                const size_t first_space = request.find(' ');
                const size_t second_space = request.find(' ', first_space + 1);
                if (first_space != std::string::npos && second_space != std::string::npos)
                {
                    path = request.substr(first_space + 1, second_space - first_space - 1);
                }
            }
            const std::string response = handler_(path);
            if (!response.empty())
            {
#ifdef __APPLE__
                const int one = 1;
                ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                // MSG_NOSIGNAL（Linux）避免对端已关闭时 SIGPIPE 杀死测试进程
                const int send_flags =
#ifdef MSG_NOSIGNAL
                    MSG_NOSIGNAL;
#else
                    0;
#endif
                ::send(client, response.data(), response.size(), send_flags);
            }
            ::close(client);
        }
    }

    std::function<std::string(const std::string&)> handler_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

TEST(CodingToolsTest, BuildDirectoryUsesProjectRootWorkspace)
{
    const auto root = std::filesystem::temp_directory_path() / "pi_coding_tools_root_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "build");
    std::ofstream(root / "CMakeLists.txt") << "project(test)\n";

    const auto tools = make_coding_tools((root / "build").string());
    const auto read_it = std::find_if(tools.begin(), tools.end(),
                                      [](const AgentTool& tool) { return tool.name == "read"; });
    ASSERT_NE(read_it, tools.end());
    const auto result = run_tool(*read_it, Json{{"path", "CMakeLists.txt"}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("project(test)"), std::string::npos);

    const auto parent_result = run_tool(*read_it, Json{{"path", "../CMakeLists.txt"}});
    ASSERT_FALSE(parent_result.content.empty());
    EXPECT_NE(parent_result.content.front().text.find("project(test)"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(CodingToolsTest, WebFetchRejectsNonHttpUrls)
{
    // 非 http(s) 协议应在发请求前被拒绝，无需访问网络
    const auto result = run_tool(web_fetch_tool(), Json{{"url", "ftp://example.com/file"}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("[web_fetch error] only http/https URLs are supported"),
              std::string::npos);
}

TEST(CodingToolsTest, WebFetchReturnsBodyAndContentType)
{
    LocalHttpServer server([](const std::string& path) -> std::string
    {
        if (path == "/hello")
        {
            return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n"
                   "Hello, world!";
        }
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    });
    const auto result = run_tool(web_fetch_tool(), Json{{"url", server.url("/hello")}});
    ASSERT_FALSE(result.content.empty());
    const auto& text = result.content.front().text;
    EXPECT_NE(text.find("status: 200"), std::string::npos);
    EXPECT_NE(text.find("content-type: text/plain"), std::string::npos);
    EXPECT_NE(text.find("Hello, world!"), std::string::npos);
    EXPECT_EQ(text.find("[web_fetch error]"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchPassesThroughHttpErrorStatus)
{
    LocalHttpServer server([](const std::string&) -> std::string
    { return "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope"; });
    const auto result = run_tool(web_fetch_tool(), Json{{"url", server.url("/missing")}});
    ASSERT_FALSE(result.content.empty());
    const auto& text = result.content.front().text;
    EXPECT_NE(text.find("status: 404"), std::string::npos);
    EXPECT_NE(text.find("nope"), std::string::npos);
    EXPECT_EQ(text.find("[web_fetch error]"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchTruncatesToUtf8CharBoundary)
{
    LocalHttpServer server([](const std::string&) -> std::string
    {
        // "你好π"= 3 字符 × 3 字节 = 9 字节；22 组 = 198 字节（66 字符），
        // 落在 max_bytes（4×50=200）硬上限之内，触发字符级截断路径；
        // 超过上限会走 "response too large" 错误而非截断
        std::string body;
        for (int i = 0; i < 22; ++i) body += "你好π";
        return "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    });
    const auto result = run_tool(web_fetch_tool(), Json{{"url", server.url("/utf8")},
                                                        {"max_chars", 50}});
    // 50 字符 = 16 组"你好π"（48 字符）+ "你好"，截断点必须落在字符边界、不切断多字节字符
    std::string expected_body;
    for (int i = 0; i < 16; ++i) expected_body += "你好π";
    expected_body += "你好";
    const std::string expected = "status: 200\ncontent-type: text/plain; charset=utf-8\n\n" +
                                 expected_body + "\n[... truncated]";
    ASSERT_FALSE(result.content.empty());
    EXPECT_EQ(result.content.front().text, expected);
}

TEST(CodingToolsTest, WebFetchFollowsRedirects)
{
    LocalHttpServer server([](const std::string& path) -> std::string
    {
        if (path == "/redirect")
        {
            return "HTTP/1.1 302 Found\r\nLocation: /hello\r\nContent-Length: 0\r\n\r\n";
        }
        if (path == "/hello")
        {
            return "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, world!";
        }
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    });
    const auto result = run_tool(web_fetch_tool(), Json{{"url", server.url("/redirect")}});
    ASSERT_FALSE(result.content.empty());
    const auto& text = result.content.front().text;
    EXPECT_NE(text.find("status: 200"), std::string::npos);
    EXPECT_NE(text.find("Hello, world!"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchAbortsWhenSignalSet)
{
    LocalHttpServer server([](const std::string&) -> std::string
    {
        // 挂住连接，让 curl 的 progress 回调观察到 abort 信号（预置为 true）
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        return "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    });
    const auto signal = std::make_shared<std::atomic<bool>>(true);
    const auto result = run_tool(web_fetch_tool(), Json{{"url", server.url("/big")}}, signal);
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("[web_fetch error] aborted"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchTimesOut)
{
    LocalHttpServer server([](const std::string&) -> std::string
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    });
    const auto result = run_tool(web_fetch_tool(),
                                 Json{{"url", server.url("/slow")}, {"timeout_seconds", 1}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("[web_fetch error] Timeout"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchAcceptsMixedCaseScheme)
{
    LocalHttpServer server([](const std::string&) -> std::string
    { return "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"; });
    // LLM 可能输出大写开头的 scheme，大小写不敏感
    const std::string url = "Http://127.0.0.1:" + std::to_string(server.port()) + "/hello";
    const auto result = run_tool(web_fetch_tool(), Json{{"url", url}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("status: 200"), std::string::npos);
}

TEST(CodingToolsTest, WebFetchRejectsInvalidArguments)
{
    // max_chars 传字符串：不得抛异常（否则整轮对话失败），应返回错误文本
    const auto result = run_tool(web_fetch_tool(),
                                 Json{{"url", "http://127.0.0.1:1/"}, {"max_chars", "abc"}});
    ASSERT_FALSE(result.content.empty());
    EXPECT_NE(result.content.front().text.find("[web_fetch error] invalid arguments"),
              std::string::npos);
}

TEST(CodingToolsTest, WebFetchLiveInternet)
{
    if (!std::getenv("PI_LIVE_NET_TESTS"))
    {
        GTEST_SKIP() << "设置 PI_LIVE_NET_TESTS=1 运行真实联网验证（默认跳过，不进 CI）";
    }
    const auto result = run_tool(web_fetch_tool(), Json{{"url", "https://example.com"}});
    ASSERT_FALSE(result.content.empty());
    const auto& text = result.content.front().text;
    EXPECT_NE(text.find("status: 200"), std::string::npos);
    EXPECT_NE(text.find("Example Domain"), std::string::npos);
}

TEST(CodingToolsTest, MapDeepSeekSearchResponse)
{
    Json response;
    response["content"] = Json::array({
        Json{{"type", "text"},
             {"text", "results"},
             {"citations",
              Json::array({Json{{"url", "https://a.com/1"}, {"cited_text", "snippet a"}},
                           Json{{"url", "https://b.com/2"}, {"cited_text", "snippet b"}}})}},
        Json{{"type", "web_search_tool_result"},
             {"content",
              Json::array({Json{{"type", "web_search_result"},
                                {"url", "https://a.com/1"},
                                {"title", "A"},
                                {"page_age", "2 days ago"}},
                           Json{{"type", "web_search_result"},
                                {"url", "https://b.com/2"},
                                {"title", "B"},
                                {"page_age", "1 hour ago"}}})}},
        // 重复 url：应按首次出现去重
        Json{{"type", "web_search_tool_result"},
             {"content", Json::array({Json{{"type", "web_search_result"},
                                           {"url", "https://a.com/1"},
                                           {"title", "A dup"}}})}},
    });

    const auto result = map_deepseek_search_response(response, 8);
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.sources.size(), 2u);
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.sources[0].url, "https://a.com/1");
    EXPECT_EQ(result.sources[0].title, "A");
    EXPECT_EQ(result.sources[0].snippet, "snippet a");
    EXPECT_EQ(result.sources[0].publishedAt, "2 days ago");
    EXPECT_EQ(result.sources[1].url, "https://b.com/2");
    EXPECT_EQ(result.sources[1].snippet, "snippet b");
    EXPECT_EQ(result.sources[1].publishedAt, "1 hour ago");
}

TEST(CodingToolsTest, MapDeepSeekSearchResponseTruncates)
{
    Json response;
    response["content"] = Json::array({Json{
        {"type", "web_search_tool_result"},
        {"content",
         Json::array({Json{{"type", "web_search_result"}, {"url", "https://a.com"}, {"title", "A"}},
                      Json{{"type", "web_search_result"}, {"url", "https://b.com"}, {"title", "B"}},
                      Json{{"type", "web_search_result"}, {"url", "https://c.com"}, {"title", "C"}}})}}});

    const auto result = map_deepseek_search_response(response, 2);
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.sources.size(), 2u);
    EXPECT_TRUE(result.truncated);
}

TEST(CodingToolsTest, MapDeepSeekSearchResponseMissingBlocksIsError)
{
    Json response;
    response["content"] = Json::array({Json{{"type", "text"}, {"text", "no results"}}});
    const auto result = map_deepseek_search_response(response, 8);
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find("no web_search_tool_result blocks"), std::string::npos);
}

TEST(CodingToolsTest, FormatSearchOutput)
{
    WebSearchResult result;
    WebSearchSource a;
    a.url = "https://a.com";
    a.title = "A Title";
    a.snippet = "snippet a";
    a.publishedAt = "2 days ago";
    WebSearchSource b;
    b.url = "https://b.com";
    b.snippet = "snippet b";  // 无标题 → hostname 兜底
    result.sources = {a, b};
    result.truncated = true;

    const std::string text = format_search_output(result);
    EXPECT_NE(text.find("- [A Title](https://a.com) — snippet a (2 days ago)"), std::string::npos);
    EXPECT_NE(text.find("- [b.com](https://b.com) — snippet b"), std::string::npos);
    EXPECT_NE(text.find("Showing the first 2 sources"), std::string::npos);
    EXPECT_NE(text.find("Cite the relevant URLs"), std::string::npos);
}

TEST(CodingToolsTest, FormatSearchOutputNoResults)
{
    WebSearchResult result;  // 空 sources 且无 answer
    const std::string text = format_search_output(result);
    EXPECT_NE(text.find("No results found."), std::string::npos);
}

}  // namespace
}  // namespace pi

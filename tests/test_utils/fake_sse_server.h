#pragma once

// 脚本化 SSE 测试服务器：POSIX socket 监听 127.0.0.1，按脚本返回 SSE 帧。
// 支持逐帧延迟（测试 abort 竞态）与请求体断言。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pi_test
{

struct FakeRequest
{
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

class FakeSseServer
{
   public:
    FakeSseServer() : running_(true)
    {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(listen_fd_);
            throw std::runtime_error("bind() failed");
        }
        if (listen(listen_fd_, 8) < 0)
        {
            close(listen_fd_);
            throw std::runtime_error("listen() failed");
        }
        socklen_t len = sizeof(addr);
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~FakeSseServer()
    {
        running_ = false;
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : connection_threads_)
        {
            if (t.joinable()) t.join();
        }
    }

    int port() const { return port_; }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

    /**
     * 脚本模式：chunks 为依次发送的原始字节（每个 chunk 后带延迟）。
     * 默认每个 chunk 已含完整 SSE 帧（含 \n\n）。
     */
    void set_script(std::vector<std::string> chunks,
                    std::chrono::milliseconds chunk_delay = std::chrono::milliseconds(0))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = Mode::Script;
        chunks_ = std::move(chunks);
        chunk_delay_ = chunk_delay;
        status_ = 200;
    }

    /**
     * 处理器模式：每次请求调用 handler(body, headers)，返回完整响应体。
     * 抛出的异常会以 500 返回。
     */
    void set_handler(
        std::function<std::string(const std::string&, const std::map<std::string, std::string>&)>
            handler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = Mode::Handler;
        handler_ = std::move(handler);
        status_ = 200;
    }

    void set_status(int status)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
    }

    std::string last_request_body()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? "" : requests_.back().body;
    }

    std::vector<FakeRequest> requests()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

   private:
    enum class Mode
    {
        Script,
        Handler
    };

    void accept_loop()
    {
        while (running_)
        {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0)
            {
                if (!running_) return;
                continue;
            }
            connection_threads_.emplace_back([this, fd] { handle_connection(fd); });
        }
    }

    void handle_connection(int fd)
    {
        std::string request_raw;
        char buf[8192];
        size_t expected = 0;
        while (true)
        {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request_raw.append(buf, n);
            if (expected == 0)
            {
                size_t header_end = request_raw.find("\r\n\r\n");
                if (header_end != std::string::npos)
                {
                    // 按 Content-Length 读完整个 body
                    size_t content_length = 0;
                    const size_t colon = request_raw.find("Content-Length:");
                    if (colon != std::string::npos && colon < header_end)
                    {
                        size_t nl = request_raw.find("\r\n", colon);
                        if (nl == std::string::npos) nl = header_end;
                        content_length =
                            std::stoul(request_raw.substr(colon + 15, nl - colon - 15));
                    }
                    expected = header_end + 4 + content_length;
                }
            }
            if (expected != 0 && request_raw.size() >= expected) break;
        }
        if (request_raw.empty())
        {
            close(fd);
            return;
        }

        FakeRequest request;
        parse_request(request_raw, request);

        std::string response_body;
        int status;
        std::chrono::milliseconds delay;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
            status = status_;
            if (mode_ == Mode::Script)
            {
                response_body.clear();
            }
            else
            {
                try
                {
                    response_body = handler_(request.body, request.headers);
                }
                catch (const std::exception& e)
                {
                    response_body = std::string("{\"error\":{\"message\":\"") + e.what() + "\"}}";
                    status = 500;
                }
            }
            delay = chunk_delay_;
        }

        std::string response =
            "HTTP/1.1 " + std::to_string(status) + " " + (status == 200 ? "OK" : "Error") + "\r\n";
        response += "Content-Type: text/event-stream\r\n";
        if (mode_ == Mode::Script)
        {
            response += "Transfer-Encoding: chunked\r\n";
        }
        else
        {
            response += "Content-Length: " + std::to_string(response_body.size()) + "\r\n";
        }
        response += "Connection: close\r\n\r\n";
        send_all(fd, response);

        if (mode_ == Mode::Script)
        {
            std::vector<std::string> chunks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                chunks = chunks_;
                delay = chunk_delay_;
            }
            for (const auto& chunk : chunks)
            {
                if (!running_) break;
                // chunked 编码帧
                std::string frame = hex(chunk.size()) + "\r\n" + chunk + "\r\n";
                if (!send_all(fd, frame)) break;
                if (delay.count() > 0) std::this_thread::sleep_for(delay);
            }
            send_all(fd, "0\r\n\r\n");
        }
        else
        {
            send_all(fd, response_body);  // 错误响应也发送 body（Content-Length 已承诺）
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    static void parse_request(const std::string& raw, FakeRequest& out)
    {
        size_t header_end = raw.find("\r\n\r\n");
        std::string head = header_end == std::string::npos ? raw : raw.substr(0, header_end);
        size_t first_nl = head.find("\r\n");
        std::string request_line = head.substr(0, first_nl);
        size_t space1 = request_line.find(' ');
        size_t space2 = request_line.find(' ', space1 + 1);
        out.method = request_line.substr(0, space1);
        out.path = request_line.substr(space1 + 1, space2 - space1 - 1);
        size_t pos = first_nl == std::string::npos ? 0 : first_nl + 2;
        while (pos < head.size())
        {
            // 最后一个 header 行没有结尾 \r\n（head 到 header_end 为止），用 head.size() 兜底
            size_t nl = head.find("\r\n", pos);
            if (nl == std::string::npos) nl = head.size();
            std::string line = head.substr(pos, nl - pos);
            if (line.empty()) break;
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ') value.erase(value.begin());
                out.headers[key] = value;
            }
            pos = nl + 2;
        }
        if (header_end != std::string::npos)
        {
            auto body_it = out.headers.find("Content-Length");
            if (body_it != out.headers.end())
            {
                size_t len = std::stoul(body_it->second);
                out.body = raw.substr(header_end + 4, len);
            }
        }
    }

    static bool send_all(int fd, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    static std::string hex(size_t value)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zx", value);
        return buf;
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{true};
    std::thread accept_thread_;
    std::vector<std::thread> connection_threads_;

    std::mutex mutex_;
    Mode mode_ = Mode::Script;
    std::vector<std::string> chunks_;
    std::chrono::milliseconds chunk_delay_{0};
    std::function<std::string(const std::string&, const std::map<std::string, std::string>&)>
        handler_;
    int status_ = 200;
    std::vector<FakeRequest> requests_;
};

/** 构造一个 SSE data 帧。 */
inline std::string sse_data(const std::string& json) { return "data: " + json + "\n\n"; }

}  // namespace pi_test

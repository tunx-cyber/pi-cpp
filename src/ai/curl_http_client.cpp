#include "curl_http_client.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdlib>

#include <algorithm>
#include <exception>

#include "pi/util/scope_exit.h"

namespace pi
{
namespace
{
// 自定义 HttpClient：abort 感知 + body augmenter（在 src 内实现 curl 部分）
class AbortableHttpClient : public openai::HttpClient
{
   public:
    AbortableHttpClient(std::shared_ptr<std::atomic<bool>> abort,
                        std::function<void(openai::json&)> augmenter)
        : abort_(std::move(abort)), augmenter_(std::move(augmenter))
    {
        static bool curl_initialized = []
        {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw openai::OpenAIError("Failed to initialize libcurl");
            return true;
        }();
        (void)curl_initialized;
    }

    openai::HttpResponse request(const openai::HttpRequest& req) override
    {
        openai::HttpRequest request = req;
        if (augmenter_ && !request.body.empty())
        {
            openai::json body = openai::json::parse(request.body);
            augmenter_(body);
            request.body = body.dump();
        }
        return do_curl(request);
    }

   private:
    struct RequestState
    {
        std::string* body;
        std::function<void(const char*, std::size_t)>* on_chunk;
        AbortableHttpClient* self;
        bool error = false;
        std::exception_ptr callback_error;
        long status = 0;
        std::string error_body;  // 流式模式下仍收集错误响应体，用于错误诊断
        std::map<std::string, std::string> headers;
    };

    static size_t write_chunk(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* ctx = static_cast<RequestState*>(userdata);
        const size_t total = size * nmemb;
        if (ctx->self->abort_->load())
        {
            ctx->error = true;
            return 0;  // 中止传输（CURLE_WRITE_ERROR）
        }
        if (ctx->status >= 400)
        {
            ctx->error_body.append(ptr, total);
            return total;
        }
        if (ctx->on_chunk && *ctx->on_chunk)
        {
            (*ctx->on_chunk)(ptr, total);
        }
        if (ctx->body)
        {
            ctx->body->append(ptr, total);
        }
        return total;
    }

    static size_t read_header(char* buffer, size_t size, size_t nitems, void* userdata)
    {
        const size_t total = size * nitems;
        std::string line(buffer, total);
        auto* state = static_cast<RequestState*>(userdata);
        if (line.rfind("HTTP/", 0) == 0)
        {
            // "HTTP/1.1 400 Bad Request" → 提取状态码
            size_t space = line.find(' ');
            if (space != std::string::npos)
            {
                state->status = std::strtol(line.c_str() + space + 1, nullptr, 10);
            }
            return total;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            auto trim = [](std::string& s)
            {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                                [](unsigned char c) { return !std::isspace(c); }));
                s.erase(std::find_if(s.rbegin(), s.rend(),
                                     [](unsigned char c) { return !std::isspace(c); })
                            .base(),
                        s.end());
            };
            trim(key);
            trim(value);
            if (!key.empty()) state->headers[key] = value;
        }
        return total;
    }

    // Exceptions must never unwind through libcurl's C callbacks.
    static size_t write_callback(char* data, size_t size, size_t count, void* userdata) noexcept
    {
        try
        {
            return write_chunk(data, size, count, userdata);
        }
        catch (...)
        {
            static_cast<RequestState*>(userdata)->callback_error = std::current_exception();
            return 0;
        }
    }

    static size_t header_callback(char* data, size_t size, size_t count, void* userdata) noexcept
    {
        try
        {
            return read_header(data, size, count, userdata);
        }
        catch (...)
        {
            static_cast<RequestState*>(userdata)->callback_error = std::current_exception();
            return 0;
        }
    }

    static int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                                 curl_off_t) noexcept
    {
        return static_cast<AbortableHttpClient*>(userdata)->abort_->load() ? 1 : 0;
    }

    openai::HttpResponse do_curl(const openai::HttpRequest& request)
    {
        CURL* curl = curl_easy_init();
        if (!curl) throw openai::OpenAIError("Failed to initialize libcurl");

        ScopeExit release_curl([&]() noexcept { curl_easy_cleanup(curl); });
        struct curl_slist* header_list = nullptr;
        ScopeExit release_headers([&]() noexcept { curl_slist_free_all(header_list); });
        for (const auto& [key, value] : request.headers)
        {
            auto* appended = curl_slist_append(header_list, (key + ": " + value).c_str());
            if (!appended) throw openai::OpenAIError("Cannot allocate HTTP headers");
            header_list = appended;
        }

        std::string response_body;
        std::function<void(const char*, std::size_t)> on_chunk = request.on_chunk;
        RequestState state{};
        state.body = request.collect_body ? &response_body : nullptr;
        state.on_chunk = on_chunk ? &on_chunk : nullptr;
        state.self = this;

        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        if (!request.body.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, request.body.size());
        }

        const CURLcode res = curl_easy_perform(curl);
        if (state.callback_error) std::rethrow_exception(state.callback_error);

        if (state.error)
        {
            if (abort_->load())
            {
                throw openai::OpenAIError("Request was aborted");
            }
            throw openai::OpenAIError("Streaming callback failed");
        }
        if (res != CURLE_OK)
        {
            if (abort_->load())
            {
                throw openai::OpenAIError("Request was aborted");
            }
            throw openai::OpenAIError(std::string("libcurl error: ") + curl_easy_strerror(res));
        }
        openai::HttpResponse response;
        response.status_code = state.status;
        response.headers = std::move(state.headers);
        // 流式模式下 collect_body=false：错误响应体仍保留，成功响应体为空
        response.body =
            request.collect_body ? std::move(response_body) : std::move(state.error_body);
        return response;
    }

    std::shared_ptr<std::atomic<bool>> abort_;
    std::function<void(openai::json&)> augmenter_;
};

}  // namespace

std::unique_ptr<openai::HttpClient> make_curl_http_client(
    std::shared_ptr<std::atomic<bool>> abort, std::function<void(openai::json&)> augmenter)
{
    return std::make_unique<AbortableHttpClient>(std::move(abort), std::move(augmenter));
}
}  // namespace pi

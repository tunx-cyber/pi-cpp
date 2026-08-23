#include "pi/ai/openai_mini.h"

#include <algorithm>
#include <thread>

namespace openai
{

namespace
{

constexpr const char* kChatCompletionsPath = "/chat/completions";
std::string build_url(const std::string& base_url, const std::string& path)
{
    if (path.empty()) return base_url;
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (!path.empty() && path.front() != '/') url.push_back('/');
    url += path;
    return url;
}

json message_content_to_json(const ChatMessageContent& content)
{
    json block = json::object();
    switch (content.type)
    {
        case ChatMessageContent::Type::Text:
            block["type"] = "text";
            block["text"] = content.text;
            break;
        case ChatMessageContent::Type::Image:
            block["type"] = "input_image";
            if (!content.image_url.empty()) block["image_url"] = content.image_url;
            break;
    }
    return block;
}

json message_to_json(const ChatMessage& message)
{
    json result;
    result["role"] = message.role;

    if (message.name) result["name"] = *message.name;
    if (message.tool_call_id) result["tool_call_id"] = *message.tool_call_id;

    // 单文本块且无工具调用时输出字符串（对齐 OpenAI Chat Completions wire 格式）。
    if (message.content.size() == 1 &&
        message.content.front().type == ChatMessageContent::Type::Text &&
        message.tool_calls.empty())
    {
        result["content"] = message.content.front().text;
    }
    else if (!message.content.empty())
    {
        json content_array = json::array();
        for (const auto& block : message.content)
        {
            content_array.push_back(message_content_to_json(block));
        }
        result["content"] = std::move(content_array);
    }

    if (!message.tool_calls.empty())
    {
        json tool_calls = json::array();
        for (const auto& call : message.tool_calls)
        {
            json tool_call = json::object();
            if (!call.id.empty()) tool_call["id"] = call.id;
            if (!call.type.empty()) tool_call["type"] = call.type;
            if (!call.function.is_null()) tool_call["function"] = call.function;
            tool_calls.push_back(std::move(tool_call));
        }
        result["tool_calls"] = std::move(tool_calls);
    }

    return result;
}

json build_chat_request_body(const ChatCompletionRequest& request, bool stream)
{
    json body;
    body["model"] = request.model;

    json messages = json::array();
    for (const auto& message : request.messages)
    {
        messages.push_back(message_to_json(message));
    }
    body["messages"] = std::move(messages);

    if (request.max_tokens) body["max_tokens"] = *request.max_tokens;
    if (request.max_completion_tokens)
        body["max_completion_tokens"] = *request.max_completion_tokens;
    if (request.temperature) body["temperature"] = *request.temperature;
    if (request.reasoning_effort) body["reasoning_effort"] = *request.reasoning_effort;

    if (!request.tools.empty())
    {
        json tools = json::array();
        for (const auto& tool : request.tools)
        {
            json tool_json = json::object();
            tool_json["type"] = tool.type;
            if (tool.function)
            {
                json fn = json::object();
                fn["name"] = tool.function->name;
                if (tool.function->description) fn["description"] = *tool.function->description;
                if (!tool.function->parameters.is_null() && !tool.function->parameters.empty())
                {
                    fn["parameters"] = tool.function->parameters;
                }
                tool_json["function"] = std::move(fn);
            }
            if (tool.raw.is_object())
            {
                for (auto it = tool.raw.begin(); it != tool.raw.end(); ++it)
                {
                    tool_json[it.key()] = it.value();
                }
            }
            tools.push_back(std::move(tool_json));
        }
        body["tools"] = std::move(tools);
    }

    if (stream) body["stream"] = true;

    if (request.stream_options)
    {
        json stream_options = json::object();
        if (request.stream_options->include_usage)
        {
            stream_options["include_usage"] = *request.stream_options->include_usage;
        }
        body["stream_options"] = std::move(stream_options);
    }

    return body;
}

std::string extract_error_message(const json& payload)
{
    if (!payload.contains("error")) return {};
    const auto& err = payload.at("error");
    if (err.is_object()) return err.value("message", "");
    if (err.is_string()) return err.get<std::string>();
    return {};
}

class SSEParser
{
   public:
    std::vector<ServerSentEvent> feed(const char* data, std::size_t size)
    {
        buffer_.append(data, size);
        return extract_events();
    }

    std::vector<ServerSentEvent> finalize()
    {
        buffer_.append("\n");
        auto events = extract_events();
        if (!current_.raw_lines.empty() || !current_.data.empty() || current_.event.has_value())
        {
            events.push_back(current_);
            current_ = ServerSentEvent{};
        }
        buffer_.clear();
        return events;
    }

   private:
    static void trim_carriage_return(std::string& line)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }

    std::vector<ServerSentEvent> extract_events()
    {
        std::vector<ServerSentEvent> events;
        std::size_t start = 0;

        while (true)
        {
            auto newline_pos = buffer_.find('\n', start);
            if (newline_pos == std::string::npos) break;

            std::string line = buffer_.substr(start, newline_pos - start);
            trim_carriage_return(line);
            start = newline_pos + 1;
            process_line(line, events);
        }

        buffer_.erase(0, start);
        return events;
    }

    void process_line(const std::string& line, std::vector<ServerSentEvent>& events)
    {
        if (line.empty())
        {
            if (!current_.raw_lines.empty() || !current_.data.empty() || current_.event.has_value())
            {
                events.push_back(current_);
                current_ = ServerSentEvent{};
            }
            return;
        }

        if (!line.empty() && line.front() == ':') return;

        current_.raw_lines.push_back(line);

        auto colon_pos = line.find(':');
        std::string field = colon_pos == std::string::npos ? line : line.substr(0, colon_pos);
        std::string value =
            colon_pos == std::string::npos ? std::string() : line.substr(colon_pos + 1);
        if (!value.empty() && value.front() == ' ') value.erase(value.begin());

        if (field == "event")
        {
            current_.event = value;
        }
        else if (field == "data")
        {
            if (!current_.data.empty()) current_.data.push_back('\n');
            current_.data += value;
        }
    }

    std::string buffer_;
    ServerSentEvent current_;
};

class SSEEventStream
{
   public:
    using EventHandler = std::function<bool(const ServerSentEvent&)>;

    explicit SSEEventStream(EventHandler handler = nullptr) : handler_(std::move(handler)) {}

    void feed(const char* data, std::size_t size)
    {
        if (stopped_) return;
        auto events = parser_.feed(data, size);
        dispatch_events(std::move(events));
    }

    void finalize()
    {
        if (stopped_) return;
        auto events = parser_.finalize();
        dispatch_events(std::move(events));
    }

   private:
    void dispatch_events(std::vector<ServerSentEvent>&& events)
    {
        if (events.empty()) return;
        for (const auto& event : events)
        {
            if (stopped_) break;
            if (handler_)
            {
                const bool should_continue = handler_(event);
                if (!should_continue) stopped_ = true;
            }
        }
    }

    SSEParser parser_;
    EventHandler handler_;
    bool stopped_ = false;
};

[[noreturn]] void throw_api_error(long status, const std::string& fallback_message)
{
    std::string message = fallback_message;
    if (message.empty()) message = "HTTP " + std::to_string(status) + " error";
    throw OpenAIError(message);
}

}  // namespace

OpenAIClient::OpenAIClient(ClientOptions options, std::unique_ptr<HttpClient> http_client)
    : options_(std::move(options)), http_client_(std::move(http_client)), chat_(*this)
{
    if (!http_client_)
    {
        throw OpenAIError("OpenAIClient requires an HttpClient implementation");
    }
    if (options_.base_url.empty()) options_.base_url = "https://api.openai.com/v1";
}

HttpResponse OpenAIClient::perform_request(const std::string& method, const std::string& path,
                                           const std::string& body,
                                           const RequestOptions& options) const
{
    const std::size_t max_retries = options.max_retries.value_or(options_.max_retries);
    std::size_t attempts = 0;

    while (true)
    {
        HttpRequest http_request;
        http_request.method = method;
        http_request.url = build_url(options_.base_url, path);
        http_request.body = body;
        http_request.timeout = options.timeout.value_or(options_.timeout);
        http_request.on_chunk = options.on_chunk;
        http_request.collect_body = options.collect_body;

        std::map<std::string, std::string> headers;
        headers["Accept"] = "application/json";
        if (!body.empty()) headers["Content-Type"] = "application/json";
        if (options_.use_bearer_auth && !options_.api_key.empty())
        {
            headers["Authorization"] = "Bearer " + options_.api_key;
        }
        for (const auto& [key, value] : options_.default_headers)
        {
            headers[key] = value;
        }
        for (const auto& [key, value] : options.headers)
        {
            headers[key] = value;
        }
        http_request.headers = std::move(headers);

        HttpResponse response;
        try
        {
            response = http_client_->request(http_request);
        }
        catch (const OpenAIError& e)
        {
            if (attempts >= max_retries) throw;
            ++attempts;
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempts));
            continue;
        }
        catch (const std::exception& e)
        {
            if (attempts >= max_retries) throw OpenAIError(e.what());
            ++attempts;
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempts));
            continue;
        }

        if (response.status_code < 400) return response;

        if ((response.status_code == 429 || response.status_code >= 500) && attempts < max_retries)
        {
            ++attempts;
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempts));
            continue;
        }

        std::string message;
        try
        {
            json payload = json::parse(response.body);
            message = extract_error_message(payload);
        }
        catch (...)
        {
        }
        throw_api_error(response.status_code, message);
    }
}

void ChatCompletionsResource::stream(const ChatCompletionRequest& request,
                                     const std::function<bool(const ServerSentEvent&)>& on_event,
                                     const RequestOptions& options) const
{
    json body = build_chat_request_body(request, true);

    RequestOptions request_options = options;
    request_options.headers["Accept"] = "text/event-stream";
    request_options.collect_body = false;

    SSEEventStream stream(on_event);
    request_options.on_chunk = [&stream](const char* data, std::size_t size)
    { stream.feed(data, size); };

    client_.perform_request("POST", kChatCompletionsPath, body.dump(), request_options);

    stream.finalize();
}

}  // namespace openai
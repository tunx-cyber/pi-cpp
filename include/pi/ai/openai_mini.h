#pragma once

#include <chrono>

#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace openai
{

using json = nlohmann::json;

class OpenAIError : public std::runtime_error
{
   public:
    explicit OpenAIError(const std::string& message) : std::runtime_error(message) {}
};

struct HttpRequest
{
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{60000};
    std::function<void(const char*, std::size_t)> on_chunk;
    bool collect_body = true;
};

struct HttpResponse
{
    long status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpClient
{
   public:
    virtual ~HttpClient() = default;
    virtual HttpResponse request(const HttpRequest& request) = 0;
};

struct ChatMessageContent
{
    enum class Type
    {
        Text,
        Image
    };

    Type type = Type::Text;
    std::string text;
    std::string image_url;
};

struct ChatCompletionToolCall
{
    std::string id;
    std::string type;
    json function = json::object();
};

struct ChatMessage
{
    std::string role;
    std::optional<std::string> tool_call_id;
    std::vector<ChatMessageContent> content;
    std::optional<std::string> name;
    std::vector<ChatCompletionToolCall> tool_calls;
};

struct ChatCompletionStreamOptions
{
    std::optional<bool> include_usage;
};

struct ChatToolFunctionDefinition
{
    std::string name;
    std::optional<std::string> description;
    json parameters = json::object();
};

struct ChatCompletionToolDefinition
{
    std::string type;
    std::optional<ChatToolFunctionDefinition> function;
    json raw = json::object();
};

struct ChatCompletionRequest
{
    std::string model;
    std::vector<ChatMessage> messages;
    std::optional<int> max_tokens;
    std::optional<int> max_completion_tokens;
    std::optional<double> temperature;
    std::optional<std::string> reasoning_effort;
    std::vector<ChatCompletionToolDefinition> tools;
    std::optional<ChatCompletionStreamOptions> stream_options;
};

struct RequestOptions
{
    std::map<std::string, std::string> headers;
    std::optional<std::size_t> max_retries;
    std::optional<std::chrono::milliseconds> timeout;
    std::function<void(const char*, std::size_t)> on_chunk;
    bool collect_body = true;
};

struct ServerSentEvent
{
    std::optional<std::string> event;
    std::string data;
    std::vector<std::string> raw_lines;
};

struct ClientOptions
{
    std::string api_key;
    std::string base_url = "https://api.openai.com/v1";
    std::chrono::milliseconds timeout{60000};
    std::size_t max_retries = 2;
    bool use_bearer_auth = true;
    std::map<std::string, std::string> default_headers;
};

class OpenAIClient;

class ChatCompletionsResource
{
   public:
    explicit ChatCompletionsResource(OpenAIClient& client) : client_(client) {}

    void stream(const ChatCompletionRequest& request,
                const std::function<bool(const ServerSentEvent&)>& on_event,
                const RequestOptions& options = {}) const;

   private:
    OpenAIClient& client_;
};

class ChatResource
{
   public:
    explicit ChatResource(OpenAIClient& client) : completions_(client) {}

    ChatCompletionsResource& completions() { return completions_; }
    const ChatCompletionsResource& completions() const { return completions_; }

   private:
    ChatCompletionsResource completions_;
};

class OpenAIClient
{
   public:
    OpenAIClient(ClientOptions options, std::unique_ptr<HttpClient> http_client = nullptr);

    ChatResource& chat() { return chat_; }
    const ChatResource& chat() const { return chat_; }

    HttpResponse perform_request(const std::string& method, const std::string& path,
                                 const std::string& body, const RequestOptions& options) const;

   private:
    ClientOptions options_;
    std::unique_ptr<HttpClient> http_client_;
    ChatResource chat_;
};

}  // namespace openai

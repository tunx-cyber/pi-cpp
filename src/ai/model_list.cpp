#include "pi/ai/model_list.h"

#include <atomic>
#include <memory>

#include "curl_http_client.h"
#include "pi/ai/json_util.h"
#include "pi/ai/openai_mini.h"

namespace pi
{

namespace
{

std::string error_message_from_payload(const Json& payload)
{
    if (!payload.is_object()) return "";
    if (payload.contains("error"))
    {
        const auto& error = payload["error"];
        if (error.is_object()) return error.value("message", "");
        if (error.is_string()) return error.get<std::string>();
    }
    if (payload.contains("message") && payload["message"].is_string())
    {
        return payload["message"].get<std::string>();
    }
    return "";
}

}  // namespace

RemoteModelList parse_model_list_response(long statusCode, const std::string& body)
{
    RemoteModelList result;
    Json payload;
    try
    {
        payload = parse_json_with_repair(body);
    }
    catch (...)
    {
        result.error = statusCode >= 400
                           ? "HTTP " + std::to_string(statusCode) + "（响应不是合法 JSON）"
                           : "响应不是合法 JSON";
        return result;
    }

    if (statusCode >= 400)
    {
        const std::string message = error_message_from_payload(payload);
        result.error = message.empty() ? "HTTP " + std::to_string(statusCode) : message;
        return result;
    }

    if (!payload.is_object() || !payload.contains("data") || !payload["data"].is_array())
    {
        result.error = "响应缺少 data 数组（不是 OpenAI 兼容的 /models 响应）";
        return result;
    }

    for (const auto& item : payload["data"])
    {
        if (!item.is_object()) continue;
        if (!item.contains("id") || !item["id"].is_string()) continue;
        RemoteModel model;
        model.id = item["id"].get<std::string>();
        if (model.id.empty()) continue;
        if (item.contains("owned_by") && item["owned_by"].is_string())
        {
            model.ownedBy = item["owned_by"].get<std::string>();
        }
        if (item.contains("created") && item["created"].is_number_integer())
        {
            model.created = item["created"].get<int64_t>();
        }
        // 去重但保留服务端顺序
        bool duplicate = false;
        for (const auto& existing : result.models)
        {
            if (existing.id == model.id)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) result.models.push_back(std::move(model));
    }
    return result;
}

RemoteModelList list_remote_models(const std::string& baseUrl, const std::string& apiKey,
                                   std::chrono::milliseconds timeout)
{
    RemoteModelList result;
    if (baseUrl.empty())
    {
        result.error = "baseUrl 为空，无法请求 /models";
        return result;
    }

    try
    {
        openai::ClientOptions options;
        options.api_key = apiKey;
        options.base_url = baseUrl;
        options.timeout = timeout;
        options.max_retries = 0;  // 列表是交互命令，失败快速返回而不是重试拖慢 UI
        options.use_bearer_auth = true;
        openai::OpenAIClient client(
            options, make_curl_http_client(std::make_shared<std::atomic<bool>>(false)));

        openai::RequestOptions request_options;
        request_options.timeout = timeout;
        request_options.max_retries = 0;
        const auto response = client.perform_request("GET", "/models", "", request_options);
        return parse_model_list_response(response.status_code, response.body);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
        return result;
    }
}

}  // namespace pi

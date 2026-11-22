#pragma once

#include <chrono>
#include <cstdint>

#include <string>
#include <vector>

namespace pi
{

/** GET /models 返回的单个模型（OpenAI 兼容格式，仅保留展示/切换所需字段）。 */
struct RemoteModel
{
    std::string id;
    std::string ownedBy;
    int64_t created = 0;
};

/** GET /models 的结果；error 为空表示成功。 */
struct RemoteModelList
{
    std::vector<RemoteModel> models;
    std::string error;
};

/**
 * 调用 OpenAI 兼容端点的 `GET {baseUrl}/models` 列出模型。
 * 不抛异常：失败时 models 为空、error 为说明（HTTP 错误/网络错误/响应格式不符）。
 * apiKey 为空时不发送 Authorization 头（兼容无需鉴权的本地端点）。
 */
RemoteModelList list_remote_models(
    const std::string& baseUrl, const std::string& apiKey,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

/**
 * 解析 GET /models 的响应（独立出来便于单测）。
 * statusCode >= 400 时提取 error.message 作为 error。
 */
RemoteModelList parse_model_list_response(long statusCode, const std::string& body);

}  // namespace pi

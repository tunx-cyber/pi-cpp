#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pi/ai/model_info.h"

namespace pi
{

/** Register a custom model override (e.g. from settings.json). Overrides built-ins. */
void register_model(const ModelInfo& model);

/** Remove a custom model override. */
void unregister_model(const std::string& id);

/** Look up a model by id (custom overrides take precedence). */
std::optional<ModelInfo> get_model(const std::string& modelId);

/** All models (built-in + overrides), keyed by provider. */
std::vector<std::string> get_providers();

/** Models for one provider. */
std::vector<ModelInfo> get_models(const std::string& provider);

/**
 * 为注册表里没有的模型 id 推断能力模板（reasoning / thinkingLevelMap / 上下文窗口等）。
 *
 * `GET /models` 只返回 id，不带能力信息；若直接把远端模型当成纯文本模型，
 * 支持思考的模型（如 deepseek-flash）会被错误地限制为 thinking=off。
 * 匹配顺序：
 *   1. id 前缀与已注册模型的 provider 一致（deepseek-flash → deepseek）；
 *   2. 端点 host 与已注册模型一致（同一个服务商）。
 * 找不到返回 nullopt，调用方应使用保守默认值（不支持思考、较小上下文窗口）。
 */
std::optional<ModelInfo> find_model_template(const std::string& modelId,
                                             const std::string& baseUrl);

/** Equality by id + provider. */
bool models_are_equal(const ModelInfo& a, const ModelInfo& b);

}  // namespace pi

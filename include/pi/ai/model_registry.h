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

/** Thinking levels supported by a model, mirroring pi's getSupportedThinkingLevels. */
std::vector<ThinkingLevel> get_supported_thinking_levels(const ModelInfo& model);

/** Clamp a requested thinking level to the nearest supported one, mirroring pi. */
ThinkingLevel clamp_thinking_level(const ModelInfo& model, ThinkingLevel level);

/** Equality by id + provider. */
bool models_are_equal(const ModelInfo& a, const ModelInfo& b);

}  // namespace pi

#pragma once

#include "pi/ai/model_info.h"

namespace pi
{

/** 纯计费计算（镜像 pi 的 calculateCost）：单价(¥/M) / 1e6 × token，结果为人民币。 */
inline Cost calculate_cost(const ModelInfo& model, const Usage& usage)
{
    Cost cost;
    cost.input = (model.costInput / 1000000.0) * static_cast<double>(usage.input);
    cost.output = (model.costOutput / 1000000.0) * static_cast<double>(usage.output);
    cost.cacheRead = (model.costCacheRead / 1000000.0) * static_cast<double>(usage.cacheRead);
    cost.cacheWrite = (model.costCacheWrite / 1000000.0) * static_cast<double>(usage.cacheWrite);
    cost.total = cost.input + cost.output + cost.cacheRead + cost.cacheWrite;
    return cost;
}

}  // namespace pi

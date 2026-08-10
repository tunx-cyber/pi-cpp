#pragma once

#include <string>

#include "pi/ai/types.h"

namespace pi
{

/** 修复字符串字面量中的控制字符与非法转义（对应 pi 的 repairJson）。 */
std::string repair_json(const std::string& json);

/** 完整解析，失败时先 repair 再重试。失败抛出 Json 异常。 */
Json parse_json_with_repair(const std::string& json);

/**
 * 容错解析流式 JSON（对应 pi 的 parseStreamingJson）。
 * 空串/解析失败时返回空对象，绝不抛异常。
 */
Json parse_streaming_json(const std::string& partial);

}  // namespace pi

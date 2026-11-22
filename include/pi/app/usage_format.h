#pragma once

#include <cstddef>
#include <cstdint>

#include <string>

#include "pi/ai/types.h"

namespace pi
{

/** 千分位分组：1234567 → "1,234,567"（负数先取绝对值，兼容 INT64_MIN）。 */
inline std::string format_token_count(int64_t value)
{
    const bool negative = value < 0;
    const uint64_t magnitude =
        negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    const std::string digits = std::to_string(magnitude);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + (negative ? 1 : 0));
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    if (negative) out.insert(out.begin(), '-');
    return out;
}

/**
 * 四项 token 用量的单行文本：input/output/cacheRead/cacheWrite/total。
 * 单价无法确定，/cost 与统计只展示 token 数量，不做金额换算。
 */
inline std::string format_token_usage(const Usage& usage)
{
    return "input=" + format_token_count(usage.input) +
           " output=" + format_token_count(usage.output) +
           " cacheRead=" + format_token_count(usage.cacheRead) +
           " cacheWrite=" + format_token_count(usage.cacheWrite) +
           " total=" + format_token_count(usage.totalTokens);
}

}  // namespace pi

#include "pi/ai/cost.h"

#include <gtest/gtest.h>

#include "pi/ai/model_registry.h"

namespace pi
{
namespace
{

TEST(CostTest, CalculatesZeroForZeroUsage)
{
    ModelInfo model;
    model.costInput = 2.0;
    model.costOutput = 8.0;
    model.costCacheRead = 0.5;
    model.costCacheWrite = 0.0;
    Usage usage;
    const Cost cost = calculate_cost(model, usage);
    EXPECT_DOUBLE_EQ(cost.input, 0);
    EXPECT_DOUBLE_EQ(cost.output, 0);
    EXPECT_DOUBLE_EQ(cost.cacheRead, 0);
    EXPECT_DOUBLE_EQ(cost.cacheWrite, 0);
    EXPECT_DOUBLE_EQ(cost.total, 0);
}

TEST(CostTest, CalculatesPerMillionTokens)
{
    ModelInfo model;
    model.costInput = 2.0;      // $2/M
    model.costOutput = 8.0;     // $8/M
    model.costCacheRead = 0.5;  // $0.5/M
    model.costCacheWrite = 1.0;
    Usage usage;
    usage.input = 1000000;
    usage.output = 500000;
    usage.cacheRead = 250000;
    usage.cacheWrite = 100000;
    usage.totalTokens = usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
    const Cost cost = calculate_cost(model, usage);
    EXPECT_DOUBLE_EQ(cost.input, 2.0);
    EXPECT_DOUBLE_EQ(cost.output, 4.0);
    EXPECT_DOUBLE_EQ(cost.cacheRead, 0.125);
    EXPECT_DOUBLE_EQ(cost.cacheWrite, 0.1);
    EXPECT_DOUBLE_EQ(cost.total, 6.225);
}

TEST(CostTest, DeepseekFlashPrices)
{
    const auto model = get_model("deepseek-v4-flash");
    ASSERT_TRUE(model.has_value());
    Usage usage;
    usage.input = 1000;
    usage.output = 2000;
    usage.cacheRead = 500;
    usage.cacheWrite = 0;
    usage.totalTokens = 3500;
    const Cost cost = calculate_cost(*model, usage);
    // 0.14/1e6*1000 + 0.28/1e6*2000 + 0.0028/1e6*500
    EXPECT_DOUBLE_EQ(cost.input, 0.00014);
    EXPECT_DOUBLE_EQ(cost.output, 0.00056);
    EXPECT_DOUBLE_EQ(cost.cacheRead, 0.0000014);
    EXPECT_DOUBLE_EQ(cost.total, 0.0007014);
}

}  // namespace
}  // namespace pi

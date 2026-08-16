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
    model.costInput = 2.0;      // ¥2/M
    model.costOutput = 8.0;     // ¥8/M
    model.costCacheRead = 0.5;  // ¥0.5/M
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

TEST(CostTest, DeepseekFlashPricesInCny)
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
    // 1.0/1e6*1000 + 2.0/1e6*2000 + 0.02/1e6*500
    EXPECT_DOUBLE_EQ(cost.input, 0.001);
    EXPECT_DOUBLE_EQ(cost.output, 0.004);
    EXPECT_DOUBLE_EQ(cost.cacheRead, 0.00001);
    EXPECT_DOUBLE_EQ(cost.total, 0.00501);
}

TEST(CostTest, PricingOverrideApplies)
{
    ModelInfo model = *get_model("deepseek-v4-flash");
    PricingOverride override;
    override.input = 9.9;
    override.output = 8.8;
    override.apply(model);
    EXPECT_DOUBLE_EQ(model.costInput, 9.9);
    EXPECT_DOUBLE_EQ(model.costOutput, 8.8);
    // 未覆盖的字段保留内置默认价
    EXPECT_DOUBLE_EQ(model.costCacheRead, 0.02);
    EXPECT_DOUBLE_EQ(model.costCacheWrite, 0.0);
}

}  // namespace
}  // namespace pi

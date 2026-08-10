#include "pi/ai/model_registry.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(ModelRegistryTest, HasDeepseekModels)
{
    const auto flash = get_model("deepseek-v4-flash");
    ASSERT_TRUE(flash.has_value());
    EXPECT_EQ(flash->provider, "deepseek");
    EXPECT_EQ(flash->baseUrl, "https://api.deepseek.com");
    EXPECT_TRUE(flash->reasoning);
    EXPECT_EQ(flash->contextWindow, 1000000);
    EXPECT_EQ(flash->maxTokens, 384000);

    const auto pro = get_model("deepseek-v4-pro");
    ASSERT_TRUE(pro.has_value());
    EXPECT_GT(pro->costOutput, flash->costOutput);
}

TEST(ModelRegistryTest, UnknownModelReturnsNullopt)
{
    EXPECT_FALSE(get_model("does-not-exist").has_value());
}

TEST(ModelRegistryTest, RegisterOverrideTakesPrecedence)
{
    const auto builtin = get_model("deepseek-v4-flash");
    ASSERT_TRUE(builtin.has_value());
    ModelInfo custom;
    custom.id = "deepseek-v4-flash";
    custom.name = "Custom Flash";
    custom.provider = "custom";
    custom.baseUrl = "http://localhost:9999";
    register_model(custom);
    const auto model = get_model("deepseek-v4-flash");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->name, "Custom Flash");
    EXPECT_EQ(model->baseUrl, "http://localhost:9999");
    unregister_model("deepseek-v4-flash");
    register_model(*builtin);  // 恢复内置模型，避免污染后续测试
}

TEST(ModelRegistryTest, SupportedThinkingLevelsDeepseek)
{
    const auto model = get_model("deepseek-v4-flash");
    ASSERT_TRUE(model.has_value());
    const auto levels = get_supported_thinking_levels(*model);
    // minimal/low/medium 显式 null → 不支持；xhigh 有映射 → 支持
    ASSERT_EQ(levels.size(), 3u);
    EXPECT_EQ(levels[0], ThinkingLevel::Off);
    EXPECT_EQ(levels[1], ThinkingLevel::High);
    EXPECT_EQ(levels[2], ThinkingLevel::Xhigh);
}

TEST(ModelRegistryTest, ClampThinkingLevelDeepseek)
{
    const auto model = get_model("deepseek-v4-flash");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(clamp_thinking_level(*model, ThinkingLevel::Medium), ThinkingLevel::High);
    EXPECT_EQ(clamp_thinking_level(*model, ThinkingLevel::High), ThinkingLevel::High);
    EXPECT_EQ(clamp_thinking_level(*model, ThinkingLevel::Xhigh), ThinkingLevel::Xhigh);
    // minimal 不支持 → 向上取高 → high
    EXPECT_EQ(clamp_thinking_level(*model, ThinkingLevel::Minimal), ThinkingLevel::High);
}

TEST(ModelRegistryTest, NonReasoningModelOnlyOff)
{
    ModelInfo model;
    model.id = "plain";
    model.reasoning = false;
    const auto levels = get_supported_thinking_levels(model);
    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(levels[0], ThinkingLevel::Off);
}

TEST(ModelRegistryTest, ModelsAreEqual)
{
    const auto a = get_model("deepseek-v4-flash");
    const auto b = get_model("deepseek-v4-flash");
    const auto c = get_model("deepseek-v4-pro");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(models_are_equal(*a, *b));
    EXPECT_FALSE(models_are_equal(*a, *c));
}

}  // namespace
}  // namespace pi

#include "pi/ai/model_registry.h"

#include <gtest/gtest.h>

#include <algorithm>

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
    EXPECT_EQ(pro->provider, flash->provider);
    EXPECT_TRUE(pro->reasoning);
}

TEST(ModelRegistryTest, UnknownModelReturnsNullopt)
{
    EXPECT_FALSE(get_model("does-not-exist").has_value());
}

TEST(ModelRegistryTest, InfersFamilyTemplateFromModelId)
{
    // GET /models 只返回 id：deepseek-flash 应继承 deepseek 家族的能力（含思考支持）
    const auto model = find_model_template("deepseek-flash", "https://proxy.example/v1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->provider, "deepseek");
    EXPECT_TRUE(model->reasoning);
    EXPECT_EQ(model->contextWindow, 1000000);
    // thinking 映射随家族继承（high→"high"、xhigh→"max"），但不再做"支持与否"判定
    ASSERT_TRUE(model->thinkingLevelMap.count(ThinkingLevel::High) > 0);
    EXPECT_EQ(model->thinkingLevelMap.at(ThinkingLevel::High), std::optional<std::string>("high"));
}

TEST(ModelRegistryTest, InfersFamilyTemplateFromEndpointHost)
{
    // id 看不出家族时，按端点 host 推断（同一个服务商共享能力）
    const auto model = find_model_template("my-endpoint-model", "https://api.deepseek.com/v1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->provider, "deepseek");
}

TEST(ModelRegistryTest, PrefersClosestFamilyMember)
{
    const auto model = find_model_template("deepseek-v4-pro-2025", "https://proxy.example/v1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->id, "deepseek-v4-pro");
}

TEST(ModelRegistryTest, UnknownFamilyHasNoTemplate)
{
    EXPECT_FALSE(find_model_template("qwen2.5-coder", "http://localhost:11434/v1").has_value());
    // 只共享字母前缀不算同一家族（缺分隔符）
    EXPECT_FALSE(find_model_template("deepseekxyz", "http://localhost:11434/v1").has_value());
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

#include "pi/ai/model_list.h"

#include <gtest/gtest.h>

#include "test_utils/fake_sse_server.h"

namespace pi
{
namespace
{

TEST(ModelListTest, ParsesOpenAiCompatibleResponse)
{
    const std::string body = R"({
        "object": "list",
        "data": [
            {"id": "deepseek-chat", "object": "model", "owned_by": "deepseek", "created": 1700000000},
            {"id": "deepseek-reasoner", "object": "model", "owned_by": "deepseek"},
            {"id": "deepseek-chat", "object": "model"}
        ]
    })";

    const auto result = parse_model_list_response(200, body);
    ASSERT_TRUE(result.error.empty());
    ASSERT_EQ(result.models.size(), 2U);
    EXPECT_EQ(result.models[0].id, "deepseek-chat");
    EXPECT_EQ(result.models[0].ownedBy, "deepseek");
    EXPECT_EQ(result.models[0].created, 1700000000);
    EXPECT_EQ(result.models[1].id, "deepseek-reasoner");
}

TEST(ModelListTest, SkipsMalformedEntriesButKeepsOrder)
{
    const std::string body = R"({"data":[
        {"id":"b"},
        {"id":""},
        {"not_id":"x"},
        "plain-string",
        {"id":7},
        {"id":"a"}
    ]})";
    const auto result = parse_model_list_response(200, body);
    ASSERT_TRUE(result.error.empty());
    ASSERT_EQ(result.models.size(), 2U);
    EXPECT_EQ(result.models[0].id, "b");
    EXPECT_EQ(result.models[1].id, "a");
}

TEST(ModelListTest, AcceptsEmptyModelList)
{
    const auto result = parse_model_list_response(200, R"({"object":"list","data":[]})");
    EXPECT_TRUE(result.error.empty());
    EXPECT_TRUE(result.models.empty());
}

TEST(ModelListTest, ReportsHttpErrorWithServerMessage)
{
    const auto result =
        parse_model_list_response(401, R"({"error":{"message":"invalid api key"}})");
    EXPECT_TRUE(result.models.empty());
    EXPECT_EQ(result.error, "invalid api key");
}

TEST(ModelListTest, ReportsInvalidResponseShape)
{
    EXPECT_EQ(parse_model_list_response(200, "not json").error, "响应不是合法 JSON");
    EXPECT_EQ(parse_model_list_response(200, R"({"object":"list"})").error,
              "响应缺少 data 数组（不是 OpenAI 兼容的 /models 响应）");
}

TEST(ModelListTest, RequestsModelsEndpointWithBearerToken)
{
    pi_test::FakeSseServer server;
    server.set_handler(
        [](const std::string&, const std::map<std::string, std::string>&)
        { return std::string(R"({"object":"list","data":[{"id":"m-one"},{"id":"m-two"}]})"); });

    const auto result =
        list_remote_models(server.base_url(), "secret-key", std::chrono::milliseconds(5000));
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.models.size(), 2U);
    EXPECT_EQ(result.models[0].id, "m-one");
    EXPECT_EQ(result.models[1].id, "m-two");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].path, "/v1/models");
    const auto auth = requests[0].headers.find("Authorization");
    ASSERT_NE(auth, requests[0].headers.end());
    EXPECT_EQ(auth->second, "Bearer secret-key");
}

TEST(ModelListTest, SurfacesHttpFailureAsError)
{
    pi_test::FakeSseServer server;
    server.set_handler([](const std::string&, const std::map<std::string, std::string>&)
                       { return std::string(R"({"error":{"message":"bad key"}})"); });
    // set_handler 会把状态重置为 200，状态需在 handler 之后设置
    server.set_status(401);

    const auto result =
        list_remote_models(server.base_url(), "secret-key", std::chrono::milliseconds(5000));
    EXPECT_TRUE(result.models.empty());
    EXPECT_EQ(result.error, "bad key");
}

}  // namespace
}  // namespace pi

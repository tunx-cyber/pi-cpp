#include "ai/curl_http_client.h"

#include <gtest/gtest.h>

#include <stdexcept>

#include "test_utils/fake_sse_server.h"

namespace pi
{
namespace
{
TEST(CurlHttpClientTest, CallbackFailureIsRethrownAndClientCanBeReused)
{
    pi_test::FakeSseServer server;
    server.set_script({"data: hello\n\n"});
    auto client = make_curl_http_client(std::make_shared<std::atomic<bool>>(false), {});
    openai::HttpRequest request;
    request.method = "POST";
    request.url = server.base_url() + "/chat/completions";
    request.on_chunk = [](const char*, size_t) { throw std::runtime_error("consumer failed"); };
    EXPECT_THROW(client->request(request), std::runtime_error);
    request.on_chunk = {};
    const auto response = client->request(request);
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "data: hello\n\n");
}

TEST(CurlHttpClientTest, BodyAugmentationFailureIsNotSilentlyIgnored)
{
    auto client =
        make_curl_http_client(std::make_shared<std::atomic<bool>>(false), [](openai::json&)
                              { throw std::runtime_error("invalid augmentation"); });
    openai::HttpRequest request;
    request.body = "{}";
    EXPECT_THROW(client->request(request), std::runtime_error);
}
}  // namespace
}  // namespace pi

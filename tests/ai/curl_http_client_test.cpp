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
    auto client = make_curl_http_client(std::make_shared<std::atomic<bool>>(false));
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

TEST(CurlHttpClientTest, PreservesErrorBodyForNonStreamingRequests)
{
    pi_test::FakeSseServer server;
    server.set_handler([](const std::string&, const std::map<std::string, std::string>&)
                       { return std::string(R"({"error":{"message":"boom"}})"); });
    server.set_status(400);

    auto client = make_curl_http_client(std::make_shared<std::atomic<bool>>(false));
    openai::HttpRequest request;
    request.method = "GET";
    request.url = server.base_url() + "/models";
    const auto response = client->request(request);
    EXPECT_EQ(response.status_code, 400);
    EXPECT_NE(response.body.find("boom"), std::string::npos);
}
}  // namespace
}  // namespace pi

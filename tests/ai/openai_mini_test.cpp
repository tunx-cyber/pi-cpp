#include "pi/ai/openai_mini.h"

#include <gtest/gtest.h>

namespace
{
class RecordingHttpClient : public openai::HttpClient
{
   public:
    openai::HttpRequest last_request;
    openai::HttpResponse request(const openai::HttpRequest& request) override
    {
        last_request = request;
        const std::string frame = "data:\ndata: hello\n\n";
        request.on_chunk(frame.data(), 7);
        request.on_chunk(frame.data() + 7, frame.size() - 7);
        return {200, {}, {}};
    }
};

TEST(OpenAiMiniTest, SerializesVisionContentForChatCompletionsAndUsesSharedSseParser)
{
    auto http = std::make_unique<RecordingHttpClient>();
    auto* recorded = http.get();
    openai::OpenAIClient client({}, std::move(http));
    openai::ChatCompletionRequest request;
    request.model = "vision-model";
    openai::ChatMessage message;
    message.role = "user";
    openai::ChatMessageContent image;
    image.type = openai::ChatMessageContent::Type::Image;
    image.image_url = "data:image/png;base64,AAAA";
    message.content.push_back(image);
    request.messages.push_back(message);
    std::vector<std::string> events;
    client.chat().completions().stream(request,
                                       [&](const openai::ServerSentEvent& event)
                                       {
                                           events.push_back(event.data);
                                           return true;
                                       });
    const auto body = openai::json::parse(recorded->last_request.body);
    EXPECT_EQ(recorded->last_request.url, "https://api.openai.com/v1/chat/completions");
    EXPECT_EQ(body["messages"][0]["content"][0],
              (openai::json{{"type", "image_url"}, {"image_url", {{"url", image.image_url}}}}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], "\nhello");
}
}  // namespace

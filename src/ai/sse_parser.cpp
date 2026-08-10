#include "pi/ai/sse_parser.h"

#include <cstring>

namespace pi
{

namespace
{

void trim_cr(std::string& line)
{
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

}  // namespace

std::vector<SseEvent> SseParser::feed(const char* data, size_t size)
{
    buffer_.append(data, size);
    return process_buffer();
}

std::vector<SseEvent> SseParser::finalize()
{
    buffer_.push_back('\n');
    auto events = process_buffer();
    if (current_started_)
    {
        events.push_back(std::move(current_));
        current_ = SseEvent{};
        current_started_ = false;
    }
    buffer_.clear();
    return events;
}

std::vector<SseEvent> SseParser::process_buffer()
{
    std::vector<SseEvent> events;
    size_t start = 0;
    while (true)
    {
        size_t nl = buffer_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = buffer_.substr(start, nl - start);
        start = nl + 1;
        trim_cr(line);

        if (line.empty())
        {
            // dispatch current event
            if (current_started_)
            {
                events.push_back(std::move(current_));
                current_ = SseEvent{};
                current_started_ = false;
            }
            continue;
        }
        if (line.front() == ':') continue;  // comment

        current_started_ = true;
        current_.rawLines.push_back(line);

        size_t colon = line.find(':');
        std::string field = colon == std::string::npos ? line : line.substr(0, colon);
        std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.erase(value.begin());

        if (field == "event")
        {
            current_.event = value;
        }
        else if (field == "id")
        {
            current_.id = value;
        }
        else if (field == "data")
        {
            if (!current_.data.empty()) current_.data.push_back('\n');
            current_.data += value;
        }
        // ignore "retry" and unknown fields
    }
    buffer_.erase(0, start);
    return events;
}

std::vector<SseEvent> parse_sse(const std::string& payload)
{
    SseParser parser;
    auto events = parser.feed(payload);
    auto rest = parser.finalize();
    events.insert(events.end(), rest.begin(), rest.end());
    return events;
}

}  // namespace pi

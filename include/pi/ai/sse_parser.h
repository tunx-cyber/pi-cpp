#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pi
{

/** One dispatched SSE event. */
struct SseEvent
{
    std::optional<std::string> event;   // "event:" field
    std::string id;                     // "id:" field
    std::string data;                   // "data:" fields joined with '\n'
    std::vector<std::string> rawLines;  // all non-comment lines
};

/**
 * Incremental SSE frame parser (RFC 8895 style): fields are data/event/id,
 * blank line dispatches the accumulated event, lines starting with ':' are
 * comments. Handles CRLF and multi-line data.
 */
class SseParser
{
   public:
    /** Feed a chunk of raw bytes; returns any completed events. */
    std::vector<SseEvent> feed(const char* data, size_t size);
    std::vector<SseEvent> feed(const std::string& data) { return feed(data.data(), data.size()); }

    /** Flush a partial trailing event (no trailing blank line). */
    std::vector<SseEvent> finalize();

   private:
    std::vector<SseEvent> process_buffer();
    std::string buffer_;
    SseEvent current_;
    bool current_started_ = false;
    bool has_data_ = false;
};

/** Parse a complete SSE payload in one call. */
std::vector<SseEvent> parse_sse(const std::string& payload);

}  // namespace pi

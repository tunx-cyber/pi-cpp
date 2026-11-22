#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "pi/ai/openai_mini.h"

namespace pi
{
std::unique_ptr<openai::HttpClient> make_curl_http_client(std::shared_ptr<std::atomic<bool>> abort);
}  // namespace pi

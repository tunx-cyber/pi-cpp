#include "pi/harness/uuid.h"

#include <chrono>
#include <cstdio>

#include <mutex>
#include <random>

namespace pi
{

namespace
{

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::mt19937_64& rng()
{
    static std::mt19937_64 rng(std::random_device{}());
    return rng;
}

uint64_t random_u64()
{
    std::uniform_int_distribution<uint64_t> dist;
    return dist(rng());
}

void format_uuid(const unsigned char bytes[16], std::string& out)
{
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    out.assign(buf);
}

}  // namespace

std::string uuidv7()
{
    // 内部状态（上次时间戳/序列号/随机引擎）非线程安全：
    // 会话条目 id 会从 run 线程与 UI 线程并发生成，必须整体串行化。
    static std::mutex generation_mutex;
    std::lock_guard<std::mutex> lock(generation_mutex);

    static int64_t last_timestamp = 0;
    static uint32_t sequence = 0;

    const int64_t timestamp = now_ms();
    uint32_t rand_word = static_cast<uint32_t>(random_u64() >> 32);
    if (timestamp > last_timestamp)
    {
        sequence = rand_word;
        last_timestamp = timestamp;
    }
    else
    {
        ++sequence;
        if (sequence == 0) ++last_timestamp;
    }

    const uint64_t random = random_u64();
    unsigned char bytes[16];
    bytes[0] = static_cast<unsigned char>(last_timestamp >> 40);
    bytes[1] = static_cast<unsigned char>(last_timestamp >> 32);
    bytes[2] = static_cast<unsigned char>(last_timestamp >> 24);
    bytes[3] = static_cast<unsigned char>(last_timestamp >> 16);
    bytes[4] = static_cast<unsigned char>(last_timestamp >> 8);
    bytes[5] = static_cast<unsigned char>(last_timestamp);
    bytes[6] = static_cast<unsigned char>(0x70 | ((sequence >> 28) & 0x0f));
    bytes[7] = static_cast<unsigned char>(sequence >> 20);
    bytes[8] = static_cast<unsigned char>(0x80 | ((sequence >> 14) & 0x3f));
    bytes[9] = static_cast<unsigned char>(sequence >> 6);
    bytes[10] = static_cast<unsigned char>(((sequence & 0x3f) << 2) | ((random >> 10) & 0x03));
    bytes[11] = static_cast<unsigned char>(random >> 2);
    bytes[12] = static_cast<unsigned char>(random >> 40);
    bytes[13] = static_cast<unsigned char>(random >> 32);
    bytes[14] = static_cast<unsigned char>(random >> 24);
    bytes[15] = static_cast<unsigned char>(random >> 16);

    std::string out;
    format_uuid(bytes, out);
    return out;
}

}  // namespace pi

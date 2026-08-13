#include "pi/app/images.h"

#include <algorithm>
#include <fstream>
#include <vector>

// stb 上游在 stb_image_write.h 里使用 sprintf，macOS SDK 将其标记为弃用；
// 该用法在上游是安全的（缓冲区长度已校验），屏蔽已知的第三方告警以保持构建输出干净
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace pi
{

std::string base64_encode(const unsigned char* data, size_t size)
{
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3)
    {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                               (i + 1 < size ? static_cast<unsigned int>(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? static_cast<unsigned int>(data[i + 2]) : 0);
        out += kTable[(n >> 18) & 0x3f];
        out += kTable[(n >> 12) & 0x3f];
        out += i + 1 < size ? kTable[(n >> 6) & 0x3f] : '=';
        out += i + 2 < size ? kTable[n & 0x3f] : '=';
    }
    return out;
}

namespace
{

constexpr int kMaxDimension = 2000;
constexpr int64_t kMaxBytes = 5 * 1024 * 1024;  // 5MB
constexpr int64_t kMaxInputPixels = 16LL * 1024 * 1024;

}  // namespace

std::optional<ContentBlock> load_image_as_block(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const std::streamsize size = file.tellg();
    if (size <= 0 || size > kMaxBytes) return std::nullopt;
    file.seekg(0);
    std::vector<unsigned char> raw(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(raw.data()), size);
    if (!file) return std::nullopt;

    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(raw.data(), static_cast<int>(raw.size()), &width, &height,
                               &channels) ||
        width <= 0 || height <= 0 || static_cast<int64_t>(width) * height > kMaxInputPixels)
    {
        return std::nullopt;
    }
    unsigned char* decoded = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &width,
                                                   &height, &channels, 4 /* force RGBA */);
    if (!decoded) return std::nullopt;

    std::vector<unsigned char> output;
    const unsigned char* pixels = decoded;
    int out_width = width;
    int out_height = height;

    const int max_dim = std::max(width, height);
    if (max_dim > kMaxDimension)
    {
        const double scale = static_cast<double>(kMaxDimension) / max_dim;
        out_width = std::max(1, static_cast<int>(width * scale));
        out_height = std::max(1, static_cast<int>(height * scale));
        output.resize(static_cast<size_t>(out_width) * out_height * 4);
        stbir_resize_uint8_linear(decoded, width, height, width * 4, output.data(), out_width,
                                  out_height, out_width * 4, STBIR_RGBA);
        pixels = output.data();
    }
    int png_size = 0;
    unsigned char* png =
        stbi_write_png_to_mem(pixels, out_width * 4, out_width, out_height, 4, &png_size);
    stbi_image_free(decoded);
    if (!png) return std::nullopt;

    ContentBlock block;
    block.type = BlockType::Image;
    block.data = base64_encode(png, static_cast<size_t>(png_size));
    // The bytes above are always re-encoded as PNG.
    block.mimeType = "image/png";
    STBIW_FREE(png);
    return block;
}

}  // namespace pi

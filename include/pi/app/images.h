#pragma once

#include <optional>
#include <string>

#include "pi/ai/types.h"

namespace pi
{

/**
 * 图片文件 → ContentBlock（≤2000px resize + base64）。
 * 镜像 pi 的图片输入管线（裁剪版，不依赖外部工具）。
 */
std::optional<ContentBlock> load_image_as_block(const std::string& filePath);

/** base64 编码。 */
std::string base64_encode(const unsigned char* data, size_t size);

}  // namespace pi

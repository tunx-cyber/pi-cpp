// openai-cpp 1.0.0 缺失 include 的统一修复，由根 CMakeLists.txt 经 -include 强制注入。
// 不要在此文件里放任何业务逻辑；新增的缺失头只追加 #include 行。
//
// 背景：openai-cpp 发布包的部分头文件依赖传递包含：
//   - webhooks.hpp 用 std::variant（缺 <variant>）
//   - utils/to_file.hpp、audio.hpp、videos.hpp 用 std::uint8_t（缺 <cstdint>）
// libc++（macOS）的传递包含掩盖了问题，GCC/libstdc++（Ubuntu CI）直接报错。
// 注意保持单个 -include 标志：CMake 会对带值选项做去重合并，两个 -include
// 会被拼成一个（第二个值被当作源文件），见 CMakeLists.txt 注释。
#include <cstdint>
#include <variant>

# nlohmann/json（vendored）

来源：[nlohmann/json](https://github.com/nlohmann/json)，固定 tag `v3.11.3`。

nlohmann/json 是 header-only 库，官方提供单头文件合并版
`single_include/nlohmann/json.hpp`，可直接拷进项目使用，无需整仓库下载。
本项目不再通过 FetchContent 拉取，改用 vendored 方式（与 stb 一致，
省去每次 configure 的联网 clone）。

包含：

- `nlohmann/json.hpp`（自包含合并头文件，约 900KB）

注：仓库还有 `single_include/nlohmann/json_fwd.hpp`（仅前置声明），
本项目没有任何文件 `#include <nlohmann/json_fwd.hpp>`，故未拷入。

许可证：MIT，版权声明完整保留在头文件顶部，未做任何修改。

## 更新方式

```bash
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git /tmp/json
cp /tmp/json/single_include/nlohmann/json.hpp third_party/nlohmann_json/nlohmann/json.hpp
# 同步更新上方 tag 号，重新编译并跑 ./build/pi_tests
```

只覆盖 `json.hpp` 一个文件即可；tests/docs/cmake 等无需拷贝。

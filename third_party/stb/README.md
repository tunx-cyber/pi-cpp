# stb（vendored）

来源：[nothings/stb](https://github.com/nothings/stb)，固定 commit `2c980bb`（master，2026-08）。

stb 是纯 header 库，官方用法就是直接把头文件拷进项目。上游没有 CMake 构建文件，
仓库不再通过 FetchContent 拉取（新版 CMake 对其报错、且每次 configure 都要联网）。

包含（仅本项目用到的 3 个）：

- `stb_image.h`（图片解码）
- `stb_image_resize2.h`（缩放）
- `stb_image_write.h`（编码）

许可证：MIT 或公共领域（二选一），版权声明完整保留在每个头文件内，未做任何修改。

## 更新方式

```bash
git clone https://github.com/nothings/stb.git /tmp/stb
cp /tmp/stb/{stb_image.h,stb_image_resize2.h,stb_image_write.h} third_party/stb/
# 同步更新上方 commit 号，重新编译并跑 ./build/pi_tests（图片用例在 tests/ 之外，
# 手工验证 /image 路径）
```

只覆盖上述 3 个文件即可；其他 stb 头文件不需要，不要整目录拷贝。

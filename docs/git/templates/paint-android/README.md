# paint-android（消费者模板）

Android UI 仓库模板。把 [paintDemo](https://github.com/KryieNaruto/paintDemo) 加为 **`sdk/`** submodule。本目录不是可运行的 App，只约定结构。

## 一次性

```bash
gh repo create KryieNaruto/paint-android --public
git clone git@github.com:KryieNaruto/paint-android.git
cd paint-android
# 从已 clone 的 SDK 调用：
/path/to/paintDemo/scripts/bootstrap-consumer.sh
```

之后：`git clone --recurse-submodules git@github.com:KryieNaruto/paint-android.git`

## 职责（路线整理 §7.5）

1. TextureView → `ANativeWindow*` → `dgcSetSurface`
2. Jetpack Ink → `dgcStrokeTo(..., isPredicted)`
3. 每帧 `dgcRender()`
4. **JNI 写在本仓库**，不要改 SDK
5. 只 `#include "dgc_paint_c_api.h"`，禁止 include `sdk/core/` 或 C++ 虚接口

## CMake

见同目录 `CMakeLists.txt`。Gradle/NDK 把 `sdk/` 编进 `libdgc_paint.so` 或使用 SDK 的 `android-arm64` 产物。

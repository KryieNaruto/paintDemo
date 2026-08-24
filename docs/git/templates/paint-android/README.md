# paint-android 消费者模板

Android UI 消费者：自备 **JNI / Compose**，把触摸事件送入 SDK C API（输入平滑已由 SDK 内核 `core/stroke_predictor` 完成），再把渲染结果合成到屏幕。

SDK 只给 **C ABI**（`dgc_paint_c_api.h`），不提供 JNI 胶水——JNI 由消费者自备。

## 目录（示意）

```text
paint-android/
├── app/
│   ├── src/main/java/...        # Compose / Activity（消费者自备）
│   ├── src/main/cpp/            # JNI 胶水（消费者自备）
│   └── CMakeLists.txt           # 见本模板（加 SDK）
├── sdk/                         # git submodule: paintDemo（路径固定 sdk/）
└── .gitmodules
```

## 接入 SDK

```bash
# 1) 先建空库（gh repo create 或网页），再 clone 进入
# 2) 加 submodule（路径固定 sdk/）
/path/to/paintDemo/scripts/bootstrap-consumer.sh --tag <tag>
# 3) 提交 .gitmodules 与 sdk 指针
git add .gitmodules sdk && git commit -m "chore: submodule paintDemo SDK 到 sdk/"
```

## CMake 约定

- 只 `add_subdirectory(sdk)`，只链接 `dgc_paint`（见 `CMakeLists.txt`）。
- 不要直接链接 SDK 内部 target，不要 include `core/`。

## JNI 职责（消费者自备）

- 把 Android `MotionEvent` 的坐标 / 压感 / tilt 转成 C API 调用：
  `dgcBeginStroke` → `dgcStrokeTo`（预测点由 SDK 内核 `stroke_predictor` 生成，消费者直接送原始点即可）→ `dgcEndStroke`。
- 把 `dgcSetSurface` 需要的 native window（ANativeWindow / Vulkan surface 句柄）从 Java 层传到 native 层。
- 把 `dgcRender` 的合成结果 present 到屏幕。
- 唯一 include：`#include "dgc_paint_c_api.h"`。

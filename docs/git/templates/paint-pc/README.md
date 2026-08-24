# paint-pc 消费者模板

PC UI 消费者：自备 **ImGui / GLFW** 窗口与输入，把鼠标 / 数位板事件送入 SDK C API，再把 `dgcRender` 结果 present 到窗口。

## 目录（示意）

```text
paint-pc/
├── src/
│   └── main.cpp               # 窗口 / 输入 / present（消费者自备）
├── CMakeLists.txt             # 见本模板（加 SDK）
├── sdk/                       # git submodule: paintDemo（路径固定 sdk/）
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

## 窗口 / 输入（消费者自备）

- 用 GLFW 创建窗口，把 HWND / window 句柄经 `dgcSetSurface` 传入 SDK。
- 鼠标 / 数位板事件转成 C API 调用：
  `dgcBeginStroke` → `dgcStrokeTo`（`isPredicted` 按消费者策略送）→ `dgcEndStroke` → `dgcRender`。
- 唯一 include：`#include "dgc_paint_c_api.h"`。

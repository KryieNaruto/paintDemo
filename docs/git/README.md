# Git 拓扑 · 基座 SDK 与消费者

本仓库 **[KryieNaruto/paintDemo](https://github.com/KryieNaruto/paintDemo)** 是绘画内核 SDK（`libdgc_paint`），**不含** Android/PC UI。

消费者把本仓库加为 git submodule，路径固定为 **`sdk/`**，CMake `add_subdirectory(sdk)`，只 `#include "dgc_paint_c_api.h"`。

## 仓库一览

| 仓库 | 角色 | 状态 |
|------|------|------|
| `KryieNaruto/paintDemo` | SDK 基座 | 已有（本仓库） |
| `KryieNaruto/paint-android` | Android 消费者（Compose + Ink + TextureView → C API） | **待建** |
| `KryieNaruto/paint-pc` | PC 消费者（GLFW + ImGui → C API） | **待建** |
| `paint-ios` / `paint-windows` | 同构消费者 | 以后 |

不做超级仓库。SDK 不反向 submodule 消费者。

```
paint-android/          paint-pc/
  sdk/  → paintDemo       sdk/  → paintDemo
  app/  Compose           src/  GLFW+ImGui
  jni/  消费者自备          （直接调 C API）
```

## 钉版本

- submodule 指向 **commit 或 tag**，不要长期跟踪漂浮的 `main`。
- 升级 SDK：在消费者里 `git -C sdk fetch && git -C sdk checkout <tag>`，再提交消费者仓库。

## 你需要先建空库

本 SDK 仓库的自动化环境 **不能** 代你执行 `gh repo create`。在 GitHub 网页或本机：

```bash
gh repo create KryieNaruto/paint-android --public --description "DGCamp Paint Android UI (consumes paintDemo SDK)"
gh repo create KryieNaruto/paint-pc --public --description "DGCamp Paint PC UI (consumes paintDemo SDK)"
```

然后 clone 空库，运行 SDK 里的 bootstrap（见下）。

## 把 SDK 加进已有空仓库

在消费者仓库根目录：

```bash
# 仓库内脚本路径以你 clone 的 SDK 为准；也可从已 clone 的 paintDemo 调用：
/path/to/paintDemo/scripts/bootstrap-consumer.sh
```

或手写：

```bash
git submodule add https://github.com/KryieNaruto/paintDemo.git sdk
git -C sdk checkout <commit-or-tag>
git add .gitmodules sdk
git commit -m "Add paintDemo SDK as submodule at sdk/"
```

克隆消费者：

```bash
git clone --recurse-submodules git@github.com:KryieNaruto/paint-android.git
```

已 clone 但没拉子模块：

```bash
git submodule update --init --recursive
```

## CMake 约定

```cmake
cmake_minimum_required(VERSION 3.22)
project(paint_android LANGUAGES C CXX)
add_subdirectory(sdk)
# target_link_libraries(your_app PRIVATE dgc_paint)
# 只允许：#include "dgc_paint_c_api.h"
```

禁止：`#include` `core/`、`kernels/`、C++ `IPaintKernel`。

## 模板

- [templates/paint-android/](templates/paint-android/)
- [templates/paint-pc/](templates/paint-pc/)

复制到新仓库后按需改名；**不要**把模板当成 SDK 仓库里的可编译 App。

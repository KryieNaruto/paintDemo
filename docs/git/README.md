# Git 远端拓扑与消费者约定

本仓库 `paintDemo` 是绘画内核 **SDK 基座**（只出 `libdgc_paint` 与唯一公开头 `sdk_api/dgc_paint_c_api.h`）。**不含 UI 消费者**——Android / PC 的窗口、输入、JNI 分别在独立仓库，通过 git submodule 引用本仓库。

## 远端拓扑

```text
┌────────────────────────────────────────────────────┐
│                KryieNaruto/paintDemo                │
│               （本仓库，SDK 基座）                    │
│   sdk_api/ · core/ · kernels/ · render/ · tests/    │
│   唯一公开头: dgc_paint_c_api.h（C ABI）             │
└───────────────┬──────────────────┬──────────────────┘
                │ submodule: sdk/  │ submodule: sdk/
                ▼                  ▼
┌─────────────────────────┐  ┌─────────────────────────┐
│ KryieNaruto/paint-android│  │  KryieNaruto/paint-pc   │
│  Android UI 消费者       │  │  PC UI 消费者           │
│  自备 JNI / Compose / Ink│  │  自备 ImGui / GLFW 窗口  │
└─────────────────────────┘  └─────────────────────────┘
```

### 待建仓库清单与用途

| 仓库 | 状态 | 用途 |
|------|------|------|
| `KryieNaruto/paintDemo` | 已存在（本仓库） | 绘画内核 SDK 基座，产出 `dgc_paint`，唯一公开头 `dgc_paint_c_api.h` |
| `KryieNaruto/paint-android` | 已建 | Android UI 消费者；自备 JNI / Compose / Ink，窗口句柄经 `dgcSetSurface` 传入 SDK；submodule 已钉 `43500e5` |
| `KryieNaruto/paint-pc` | 已建 | PC UI 消费者；自备 ImGui / GLFW 窗口，窗口句柄经 `dgcSetSurface` 传入 SDK；submodule 已钉 `43500e5` |

> 消费者仓库不在本仓库创建（由独立 GitHub 仓库承载）。`paint-android` / `paint-pc` 骨架已搭建，submodule 引用本库，窗口 / 输入 / JNI 仍由消费者实现。

## 消费约定（必须遵守）

1. **submodule 路径固定 `sdk/`**，远端指向
   `https://github.com/KryieNaruto/paintDemo.git`：

   ```bash
   git submodule add https://github.com/KryieNaruto/paintDemo.git sdk
   ```

   也可用 `scripts/bootstrap-consumer.sh` 一键完成（见下文）。

2. **钉 commit 或 tag，禁止长期漂浮跟踪 `main`。**
   接入后立刻 `git -C sdk checkout <tag|commit>` 并把 submodule 记录提交进消费者仓库。
   SDK 更新需显式、经评审地重钉版本，而不是自动跟随 `main` 漂移。

3. **消费者只 `#include "dgc_paint_c_api.h"`，禁止 include `core/`。**
   `core/` `kernels/` `render/` 都是 SDK 内部实现，不在 ABI 承诺内；
   唯一稳定公开面是 `sdk_api/dgc_paint_c_api.h`。

4. **clone 消费者仓库时用 `--recurse-submodules`：**

   ```bash
   git clone --recurse-submodules git@github.com:KryieNaruto/paint-android.git
   ```

   已 clone 未拉子模块时补：`git submodule update --init --recursive`。

## 一键接入（bootstrap）

脚本：`scripts/bootstrap-consumer.sh`（本仓库根目录下）。

```bash
# 1) 先自己在 GitHub 建一个【空】消费者仓库：
gh repo create KryieNaruto/paint-android --private   # 或网页 https://github.com/new
# 2) clone 空库并进入：
git clone git@github.com:KryieNaruto/paint-android.git && cd paint-android
# 3) 在该空库目录里执行（脚本只操作当前消费者目录，不碰 SDK 仓库文件）：
/path/to/paintDemo/scripts/bootstrap-consumer.sh --tag v0.1.0
# 4) 提交 .gitmodules 与 submodule 指针：
git add .gitmodules sdk && git commit -m "chore: submodule paintDemo SDK 到 sdk/"
```

`--help` 会再次说明「先自己 `gh repo create` / 网页建空库」的前置条件。

## 消费者工程模板

- `templates/paint-android/`：Android UI 消费者模板（JNI / Compose / Ink 自备）。
- `templates/paint-pc/`：PC UI 消费者模板（ImGui / GLFW 窗口自备）。

模板 CMake 只做 `add_subdirectory(sdk)` 并只链接 `dgc_paint`，不写窗口 / 输入逻辑。

## 非范围（不进本 SDK 仓库）

- 消费者仓库（`paint-android` / `paint-pc`）不在本仓库创建。
- `ui/` `app/` `platform/` 代码不进本 SDK 仓库；窗口、输入、JNI 全部由消费者自备。

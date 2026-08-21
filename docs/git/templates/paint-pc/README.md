# paint-pc（消费者模板）

PC UI 仓库模板。把 [paintDemo](https://github.com/KryieNaruto/paintDemo) 加为 **`sdk/`** submodule。本目录不是可运行的桌面程序。

## 一次性

```bash
gh repo create KryieNaruto/paint-pc --public
git clone git@github.com:KryieNaruto/paint-pc.git
cd paint-pc
/path/to/paintDemo/scripts/bootstrap-consumer.sh
```

之后：`git clone --recurse-submodules git@github.com:KryieNaruto/paint-pc.git`

## 职责（路线整理 §7.5）

1. GLFW（或其它）创建窗口 → 原生句柄 → `dgcSetSurface`
2. 鼠标/数位笔 → `dgcStrokeTo`
3. 每帧 `dgcRender()`
4. ImGui 只调 C API（设笔刷/清屏），不 include `core/`

GLFW / ImGui 依赖安装在**本消费者**，不是 SDK 的 E0 必需项。

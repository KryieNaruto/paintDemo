# DGCamp Paint 原型

技术验证型原型：用 **libmypaint + Jetpack Ink + Vulkan Compute Shader** 验证能否达到 Procreate 级别的绘画手感。目标平台 Android 平板。

- 技术规划 → [`DGCPaint_技术规划.md`](DGCPaint_技术规划.md)
- 任务与进度 → [`docs/任务线.md`](docs/任务线.md)

## 换设备 / 新设备上手

### 1. clone（公开仓库，clone 无需认证）

```bash
git clone git@github.com:KryieNaruto/paintDemo.git      # SSH（推送需要）
# 或只读 https：
git clone https://github.com/KryieNaruto/paintDemo.git
```

> SSH 推送需要把新设备的公钥（`~/.ssh/id_ed25519.pub`）加到 GitHub 账号。

### 2. 设 git 身份

```bash
cd paintDemo
git config user.name  "<你的名字>"
git config user.email "<你的邮箱>"
```

### 3. 验证任务线系统（自包含，随 clone 一起到）

```bash
python3 .exec/taskline.py status   # 应显示 15 条任务、首波可领 3 条（T0-1/T0-2/T1-1）
```

### 4. 开一条任务

任意终端说「开工 / 领任务」或 `/paint-dev`，进入 5 阶段流水线（申领 → 计划 → 执行 → 测试 → 收尾 + 评审）。
每个新会话开局会自动刷任务简报（`.claude/settings.json` 的 SessionStart hook）。

## 环境前置（人工，见 `docs/任务线.md`「起点条件」）

- Android Studio Ladybug+ / NDK r27+ / CMake 3.31 / JDK 21
- VS2026（C++ 桌面开发 + CMake tools）+ LunarG Vulkan SDK
- 物理测试平板（Galaxy Tab S9+/S10+ 等）+ 开发者模式 / USB 调试

详见 `DGCPaint_技术规划.md` §1。

## 目录

| 路径 | 说明 |
|---|---|
| `docs/任务线.md` | 任务状态与依赖的唯一权威（SOT） |
| `.exec/taskline.py` | 查询 / 认领 / 收尾脚本 |
| `.claude/skills/paint-dev/` | 编排 SKILL（5 阶段流水线） |
| `.claude/agents/` | 6 个阶段 agent（claim/plan/execute/test/review/finish） |

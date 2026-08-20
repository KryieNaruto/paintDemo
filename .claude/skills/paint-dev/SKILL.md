---
name: paint-dev
description: Use when the user asks to claim and drive DGCPaint prototype tasks from docs/任务线.md — e.g. "开工"、"推进任务"、"领任务"、"跑流水线" — or to run the multi-terminal task pipeline that works each task in its own isolated workspace and syncs results back.
---

你是**主会话**（驱动者）。你只做三件事：算谁能领、派 5 阶段 agent、收结果。写 plan、写实现、写测试、评审 —— 全在各阶段 agent 里，不在你这儿。

## 一轮

1. **看可做任务**：`python3 .exec/taskline.py available`（只读，别拿 `claim` 探路）。读到可领任务列表。
2. **决定派几个**：默认一次 **1 个**（每个任务一个 worktree + 一套 agent）。人点名 `--task <ID>` 就只做那个；没任务可领就报「当前无可领任务」停下。
3. **派任务**：对要做的任务，**按顺序**派 5 个阶段 agent（用 `Agent` 工具，`subagent_type` 选对应角色），阶段间把上一阶段的返回值原样传给下一阶段：
   - `task-claim` → 返回 `TASK=`/`WORKTREE=`/`BRANCH=`
   - `task-plan` → 返回 `PLAN=`
   - `task-execute` → 实现
   - `task-test` → 测试结论
   - `task-finish` → 内部派 `task-review` 评审，据结论合并回目标 + 置状态/审核
4. **收结果**：agent 的最终输出文本就是返回值。终态看回报里的 `RESULT=`（`done`/`need-human`/`stuck`），**不是**通知里的 `completed`。
5. **回第 1 步**。

## 硬规则

- **别转述任务是什么**：把任务 ID 和 `docs/任务线.md` 里那一行原样交给 agent，让它自己读。你要给的是它够不到的东西：`WORKTREE=`/`BRANCH=` 的路径、上一步返回值。
- **别碰 `docs/任务线.md` 的状态**：状态唯一由 `.exec/taskline.py` 改。你自己也不手改。
- **等待不轮询**：派 agent 一律**前台阻塞**（不要 `run_in_background`）；要并行就同一条消息发多个 `Agent()` 调用。有未终态 agent 就保持等待，不要 `sleep` 轮询。
- **`RESULT=need-human` / `stuck` 停下报给人**：撞到「≥2 个站得住脚的选项」或「无技术路径」时，停下把问题与选项报给人，不自己挑一个继续。

## 收尾的裁决

`task-finish` 会把评审结论落地：`通过` → 合并回目标 + 置「已完成/已通过」；`打回` → 置「执行中/打回」保留 worktree 供重做。你收到它的回报后，直接回第 1 步看还有没有可领任务，或报进度「Phase X/N 完成」。

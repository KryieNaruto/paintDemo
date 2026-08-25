---
name: paint-dev
description: Use when the user asks to claim and drive DGCPaint SDK tasks from docs/tasks/任务线.md — e.g. "开工"、"推进任务"、"领任务"、"跑流水线" — or to run the multi-terminal task pipeline that works each task in its own isolated workspace and syncs results back.
---

你是**主会话**（驱动者）。你只做三件事：算谁能领、派带评分门禁的阶段 agent、收结果。写 plan、写实现、写测试、评审、打分 —— 全在各阶段 agent 里，不在你这儿。

## 一轮

1. **看可做任务**：`python3 .exec/taskline.py available`（只读，别拿 `claim` 探路）。读到可领任务列表，**汇报给人工**，不擅自开领。
2. **决定派几个**：默认一次 **1 个**（每个任务一个 worktree + 一套 agent）。人点名 `--task <ID>` 就只做那个；没人指示申领就不领，停下等人。没任务可领就报「当前无可领任务」停下。
3. **派任务**：对要做的任务，**按顺序**派带评分门禁的 7 步阶段 agent（用 `Agent` 工具，`subagent_type` 选对应角色），阶段间把上一阶段的返回值原样传给下一阶段：
   - `task-claim` → 返回 `TASK=`/`WORKTREE=`/`BRANCH=`
   - `task-plan` → 返回 `PLAN=`
   - `task-plan-review` → **plan 门禁**：按 rubric 打分，`SCORE>80` 才通过（返回 `SCORE=`/`VERDICT=`/`FEEDBACK=`）。`<=80` → 把 `FEEDBACK=` 原样传回 `task-plan` 重写 → 再派 `task-plan-review` 重评，**最多 3 轮**；仍不达 → `need-human` 停下报人。
   - `task-execute` → 实现
   - `task-test` → 测试结论
   - `task-test-review` → **test 门禁**：按 rubric 打分，`SCORE==100` 才通过（返回 `SCORE=`/`VERDICT=`/`FEEDBACK=`）。`<100` → 把 `FEEDBACK=` 原样传回 `task-execute` 修复 → 重派 `task-test` → `task-test-review`，**最多 3 轮**；仍不达 → `need-human` 停下报人。
   - `task-finish` → 内部派 `task-review` 评审（无 Task 工具的环境下它会内联完成同等评审取证，引用前两门禁分数为证据），据结论合并回目标 + 置状态/审核
4. **收结果**：agent 的最终输出文本就是返回值。终态看回报里的 `RESULT=`（`done`/`need-human`/`stuck`），**不是**通知里的 `completed`。
5. **停下等人工**：一个任务彻底结束后（收尾已落地），汇报结果并**停下等人工审核/指示，不自动回第 1 步申领下一个**。只有人工说「继续/推进」才回第 1 步。

## 恢复会话 / 接管既有 worktree

会话恢复或发现某任务已处于「执行中」时，**先查再派，别机械重跑**：

- 跑 `git log --oneline -5` + `git worktree list` + 看 `docs/plans/` 判断每个执行中任务的进度：已 claim 未 plan → 从 `task-plan` 接手；已 plan 未过 plan-review（或 plan-review 低分待重写）→ 从 `task-plan` / `task-plan-review` 接手；已过 plan-review 未实现 → 从 `task-execute` 接手；已实现未测试 → 从 `task-test` 接手；已测试未过 test-review → 从 `task-execute`（修复）→ `task-test` → `task-test-review` 接手；已过 test-review 未收尾 → 从 `task-finish` 接手。
- 用 `python3 .exec/taskline.py status`（或 available）核实任务线状态，别只信 agent 回报——本会话曾出现测试阶段任务已被并发流程收尾、plan 文件写好未提交等情况。
- 若发现某任务已被并发完成（`git log` 见 `finish <ID>`），直接跳过该任务的剩余阶段，不要再派 agent。

## 硬规则

- **别转述任务是什么**：把任务 ID 和 `docs/tasks/任务线.md` 里那一行原样交给 agent，让它自己读。你要给的是它够不到的东西：`WORKTREE=`/`BRANCH=` 的路径、上一步返回值。
- **别碰 `docs/tasks/任务线.md` 的状态**：状态唯一由 `.exec/taskline.py` 改。你自己也不手改。验收标准从 `docs/tasks/detail/` 对应任务书抄，不自编。
- **等待不轮询**：派 agent 一律**前台阻塞**（不要 `run_in_background`）；要并行就同一条消息发多个 `Agent()` 调用。有未终态 agent 就保持等待，不要 `sleep` 轮询。
- **IDE clang 诊断是误报口**：host 无 compile_commands.json 时，IDE/clangd 会报 `pp_file_not_found`/`unknown_typename` 等一屏假错，实际 `g++ -I.` / `cmake` 构建是通过的。看到此类诊断别当实现问题追，以实际构建 + ctest 为准；传给 `task-test` 的提示里带上「以实际构建为准」口径。
- **SDK 工程约束（RAII/Pimpl/零泄漏）**：SDK（dgc_paint 库）内所有权一律 RAII，禁止裸 `new`/`delete` 所有权（一律 `make_unique`/`unique_ptr`）；SDK 对外功能一律经不透明句柄 Pimpl 隐藏实现（C ABI 不暴露任何内部类型）。派给 `task-plan`/`task-execute`/`task-test`/`task-review`/`task-plan-review`/`task-test-review` 的提示都带上此口径；两个评审 agent 的 rubric 里各有对应分项。
- **`RESULT=need-human` / `stuck` 停下报给人**：撞到「≥2 个站得住脚的选项」或「无技术路径」时，停下把问题与选项报给人，不自己挑一个继续。
- **收尾后停下等人工，不自动申领**：`task-finish` 报回终态后，汇报并停下等人工审核。只有人工明确说「继续/推进」才回第 1 步看 `available` 并申领。别拿「还有可领任务」当继续的理由——可领 ≠ 该领。

## 收尾的裁决

`task-finish` 只在 plan-review（>80）与 test-review（==100）两门禁都通过后才派发，它把最终评审结论落地：`通过` → 合并回目标 + 置「已完成/已通过」；`打回` → 置「执行中/打回」保留 worktree 供重做。你收到它的回报后，汇报任务结果并**停下等人工审核**，不自动回第 1 步申领下一个任务。人工说「继续」再回第 1 步。

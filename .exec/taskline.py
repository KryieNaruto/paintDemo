#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DGCPaint 任务线查询 / 认领 / 收尾脚本。

唯一真相源：docs/任务线.md 的任务表（状态与依赖）。状态只经本脚本修改，不要手改表格。

状态机：
  可申领 --claim--> 执行中 --finish(通过)--> 已完成
                 \\--finish(打回)--> 执行中(审核=打回)
  执行中 --release--> 可申领
  执行中 --set--> 阻塞

认领防冲突：同机多会话靠 flock；多机靠「置状态后 commit + push」乐观锁（谁先 push 谁赢）。
"""

import argparse
import contextlib
import os
import re
import subprocess
import sys

try:
    import fcntl
except ImportError:  # 无 fcntl 的平台退化为无本地锁（仍靠 git push 乐观锁）
    fcntl = None

STATUSES = ("可申领", "执行中", "阻塞", "已完成")
AUDITS = ("待审核", "已通过", "打回")
DEFAULT_BRANCHES = ("main", "master")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = None
TASKS_MD = None
LOCK_FILE = None
WORKTREE_DIR = None


def find_root():
    d = os.path.abspath(SCRIPT_DIR)
    while True:
        if os.path.isfile(os.path.join(d, "docs", "任务线.md")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    env = os.environ.get("DGCPAIN_ROOT")
    if env and os.path.isfile(os.path.join(env, "docs", "任务线.md")):
        return env
    return None


def _init_paths():
    global ROOT, TASKS_MD, LOCK_FILE, WORKTREE_DIR
    ROOT = find_root() or os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
    TASKS_MD = os.path.join(ROOT, "docs", "任务线.md")
    LOCK_FILE = os.path.join(ROOT, ".exec", ".taskline.lock")
    WORKTREE_DIR = os.path.join(ROOT, ".worktrees")


def git(*args, cwd=None):
    return subprocess.run(
        ["git", "-C", cwd or ROOT, *args], capture_output=True, text=True
    )


def git_ok(*args, cwd=None):
    return git(*args, cwd=cwd).returncode == 0


def default_branch():
    r = git("symbolic-ref", "--short", "HEAD")
    b = r.stdout.strip()
    if b:
        return b
    for b in DEFAULT_BRANCHES:
        if git_ok("rev-parse", "--verify", b):
            return b
    return "main"


# ── 解析 / 渲染 ──────────────────────────────────────────────

def split_deps(s):
    if not s or s.strip() in ("—", "-", "无", ""):
        return []
    return [x for x in re.split(r"[、,，\s/]+", s.strip()) if x]


def parse_tasks(text):
    rows = []
    in_table = False
    for ln in text.splitlines():
        s = ln.strip()
        if not in_table:
            if s.startswith("| ID") or s.startswith("|ID"):
                in_table = True
            continue
        if not s.startswith("|"):
            in_table = False
            continue
        if all(ch in "|-: " for ch in s):
            continue  # 分隔行 |---|
        cells = [c.strip() for c in s.strip().strip("|").split("|")]
        if len(cells) >= 6 and cells[0]:
            rows.append({
                "id": cells[0],
                "line": cells[1],
                "name": cells[2],
                "status": cells[3],
                "deps": split_deps(cells[4]),
                "audit": cells[5],
            })
    return rows


def read_tasks():
    if not os.path.isfile(TASKS_MD):
        return []
    with open(TASKS_MD, encoding="utf-8") as f:
        return parse_tasks(f.read())


def find(tasks, tid):
    for t in tasks:
        if t["id"] == tid:
            return t
    return None


def status_of(tasks, tid):
    t = find(tasks, tid)
    return t["status"] if t else None


def deps_satisfied(tasks, t):
    return all(status_of(tasks, d) == "已完成" for d in t["deps"])


def render_summary(tasks):
    n = {"可申领": 0, "执行中": 0, "阻塞": 0, "已完成": 0}
    for t in tasks:
        n[t["status"]] = n.get(t["status"], 0) + 1
    return (f"总 {len(tasks)} · 可申领 {n['可申领']} · 执行中 {n['执行中']} "
            f"· 阻塞 {n['阻塞']} · 已完成 {n['已完成']}")


def write_tasks(tasks):
    with open(TASKS_MD, encoding="utf-8") as f:
        lines = f.read().splitlines()
    summary = render_summary(tasks)
    out = []
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s == "<!-- SUMMARY-BEGIN -->":
            out.append(lines[i]); i += 1
            while i < len(lines) and lines[i].strip() != "<!-- SUMMARY-END -->":
                i += 1
            out.append(summary)
            out.append(lines[i]); i += 1
        elif s.startswith("| ID") or s.startswith("|ID"):
            out.append(lines[i]); i += 1
            if i < len(lines):  # 分隔行
                out.append(lines[i]); i += 1
            while i < len(lines) and lines[i].strip().startswith("|"):
                i += 1
            for t in tasks:
                deps = "、".join(t["deps"]) if t["deps"] else "—"
                out.append(
                    f"| {t['id']} | {t['line']} | {t['name']} | {t['status']} | {deps} | {t['audit']} |"
                )
        else:
            out.append(lines[i]); i += 1
    with open(TASKS_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


@contextlib.contextmanager
def lock():
    if fcntl is None:
        yield
        return
    os.makedirs(os.path.dirname(LOCK_FILE), exist_ok=True)
    fd = open(LOCK_FILE, "a+")
    try:
        fcntl.flock(fd.fileno(), fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(fd.fileno(), fcntl.LOCK_UN)
        finally:
            fd.close()


def commit_and_push(msg):
    if not git_ok("add", "docs/任务线.md"):
        sys.exit("git add 失败")
    r = git("commit", "-m", msg)
    if r.returncode != 0:
        if "nothing to commit" in r.stdout + r.stderr:
            return
        sys.exit(f"git commit 失败：{r.stderr}")
    if git_ok("remote", "get-url", "origin"):
        br = default_branch()
        for _ in range(3):
            r = git("push", "origin", br)
            if r.returncode == 0:
                return
            git("pull", "--rebase", "origin", br)
        sys.exit(f"push 失败（可能被别的终端抢先）：{r.stderr}")


# ── 查询 ─────────────────────────────────────────────────────

def fmt_task(t):
    return f"  {t['id']:<6}{t['line']:<12}{t['name']}  [审核:{t['audit']}]"


def print_section(title, items):
    print(f"== {title}（{len(items)}）==")
    if not items:
        print("  （空）")
    for t in items:
        print(fmt_task(t))
    print()


def cmd_status():
    tasks = read_tasks()
    if not tasks:
        print("（docs/任务线.md 尚未就绪）")
        return
    print(render_summary(tasks))
    print()
    print_section("可申领（依赖已满足，可立即领）",
                  [t for t in tasks if t["status"] == "可申领" and deps_satisfied(tasks, t)])
    print_section("执行中", [t for t in tasks if t["status"] in ("执行中", "阻塞")])
    print_section("已完成", [t for t in tasks if t["status"] == "已完成"])
    waiting = [t for t in tasks if t["status"] == "可申领" and not deps_satisfied(tasks, t)]
    if waiting:
        print("== 等待依赖（状态可申领但依赖未完成）==")
        for t in waiting:
            missing = [d for d in t["deps"] if status_of(tasks, d) != "已完成"]
            print(f"  {t['id']:<6}{t['name']}  等：{'、'.join(missing)}")
        print()


def cmd_available():
    tasks = read_tasks()
    for t in tasks:
        if t["status"] == "可申领" and deps_satisfied(tasks, t):
            print(fmt_task(t))


def cmd_running():
    tasks = read_tasks()
    for t in tasks:
        if t["status"] in ("执行中", "阻塞"):
            print(fmt_task(t))


def cmd_done():
    tasks = read_tasks()
    for t in tasks:
        if t["status"] == "已完成":
            print(fmt_task(t))


def cmd_peek():
    tasks = read_tasks()
    for t in tasks:
        if t["status"] == "可申领" and deps_satisfied(tasks, t):
            print(fmt_task(t))
            return
    print("（当前无可领任务）")


# ── 变更 ─────────────────────────────────────────────────────

def cmd_claim(tid):
    with lock():
        tasks = read_tasks()
        t = find(tasks, tid)
        if not t:
            sys.exit(f"任务 {tid} 不存在")
        if t["status"] != "可申领":
            sys.exit(f"任务 {tid} 状态是「{t['status']}」，不可申领")
        missing = [d for d in t["deps"] if status_of(tasks, d) != "已完成"]
        if missing:
            sys.exit(f"任务 {tid} 依赖未完成，等：{'、'.join(missing)}")
        os.makedirs(WORKTREE_DIR, exist_ok=True)
        branch = f"task/{tid}"
        wt = os.path.join(WORKTREE_DIR, tid)
        r = git("worktree", "add", "-b", branch, wt)
        if r.returncode != 0:
            sys.exit(f"git worktree add 失败：{r.stderr}")
        t["status"] = "执行中"
        write_tasks(tasks)
        commit_and_push(f"claim {tid}: 可申领 → 执行中")
        print(f"TASK={tid}")
        print(f"BRANCH={branch}")
        print(f"WORKTREE={wt}")
        print(f"TITLE={t['name']}")


def cmd_finish(tid, audit):
    if audit not in ("通过", "打回"):
        sys.exit("--audit 只接受 通过|打回")
    with lock():
        tasks = read_tasks()
        t = find(tasks, tid)
        if not t:
            sys.exit(f"任务 {tid} 不存在")
        if t["status"] not in ("执行中", "阻塞"):
            sys.exit(f"任务 {tid} 状态是「{t['status']}」，不能收尾")
        branch = f"task/{tid}"
        wt = os.path.join(WORKTREE_DIR, tid)
        if audit == "通过":
            if git_ok("rev-parse", "--verify", branch):
                r = git("merge", "--no-ff", branch, "-m", f"合并 {tid}: {t['name']}")
                if r.returncode != 0:
                    sys.exit(f"merge 失败（可能冲突）：{r.stderr}\n在 worktree 解决冲突后重跑 finish")
            git("worktree", "remove", "--force", wt)
            git("branch", "-D", branch)
            t["status"] = "已完成"
            t["audit"] = "已通过"
        else:
            t["status"] = "执行中"
            t["audit"] = "打回"
        write_tasks(tasks)
        commit_and_push(f"finish {tid}: → {t['status']} / {t['audit']}")
        print(f"TASK={tid}")
        print(f"STATUS={t['status']}")
        print(f"AUDIT={t['audit']}")


def cmd_release(tid):
    with lock():
        tasks = read_tasks()
        t = find(tasks, tid)
        if not t:
            sys.exit(f"任务 {tid} 不存在")
        if t["status"] not in ("执行中", "阻塞"):
            sys.exit(f"任务 {tid} 状态是「{t['status']}」，无需退回")
        wt = os.path.join(WORKTREE_DIR, tid)
        git("worktree", "remove", "--force", wt)
        git("branch", "-D", f"task/{tid}")
        t["status"] = "可申领"
        write_tasks(tasks)
        commit_and_push(f"release {tid}: → 可申领")
        print(f"TASK={tid} 已退回可申领")


def cmd_set(tid, status):
    with lock():
        tasks = read_tasks()
        t = find(tasks, tid)
        if not t:
            sys.exit(f"任务 {tid} 不存在")
        t["status"] = status
        write_tasks(tasks)
        commit_and_push(f"set {tid}: → {status}")
        print(f"TASK={tid} → {status}")


def main():
    _init_paths()
    p = argparse.ArgumentParser(description="DGCPaint 任务线查询/认领/收尾")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="三栏显示 可申领/执行中/已完成")
    sub.add_parser("available", help="可申领（依赖已满足）")
    sub.add_parser("running", help="执行中（含阻塞）")
    sub.add_parser("done", help="已完成")
    sub.add_parser("peek", help="只读展示下一个可领任务")

    pc = sub.add_parser("claim", help="认领任务（开 worktree + 置执行中）")
    pc.add_argument("id")

    pf = sub.add_parser("finish", help="收尾任务（合并回目标 + 置状态/审核）")
    pf.add_argument("id")
    pf.add_argument("--audit", choices=["通过", "打回"], required=True)

    pr = sub.add_parser("release", help="退回任务到可申领")
    pr.add_argument("id")

    ps = sub.add_parser("set", help="手动设状态")
    ps.add_argument("id")
    ps.add_argument("status", choices=STATUSES)

    args = p.parse_args()

    if args.cmd == "status":
        cmd_status()
    elif args.cmd == "available":
        cmd_available()
    elif args.cmd == "running":
        cmd_running()
    elif args.cmd == "done":
        cmd_done()
    elif args.cmd == "peek":
        cmd_peek()
    elif args.cmd == "claim":
        cmd_claim(args.id)
    elif args.cmd == "finish":
        cmd_finish(args.id, args.audit)
    elif args.cmd == "release":
        cmd_release(args.id)
    elif args.cmd == "set":
        cmd_set(args.id, args.status)


if __name__ == "__main__":
    main()

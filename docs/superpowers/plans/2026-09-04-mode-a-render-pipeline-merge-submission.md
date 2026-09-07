# Mode A 渲染管线合并提交 + 预测线 stale-tip 修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把「合并 GPU 提交」（4a）与「预测批 composite 顺序修复」（4c）落地，砍掉每次读回背后
多余的 GPU 提交往返，并让预测尖的可见内容不再滞后一批，为 4d（真机重新验证）铺路。

**Architecture:** 两个独立、低风险的白盒改动：(1) `core/engine.cpp` 的 `flushAccum` 把预测批
composite 顺序调整到真实批之前，从一个新提取的、可独立单测的自由函数 `CompositeOrdered` 落地；
(2) `render/vulkan/vk_backend.cpp` 把 `RefreshReadbackCacheLocked()` 拆成「录制 GPU 命令」
（`RecordRefreshCommands`）与「提交后处理」（`FinishRefreshReadback`）两半，`CompositeLocked()`
在自己的 `SubmitAndWait()` 之前顺带录制刷新命令，省掉一次独立提交；`ClearCanvasLocked`/
`ClearTipLocked`/`flushReadbackCache()` 三个既有独立调用点保持不变（各自仍是 Begin→Record→
Submit→Finish 一整套）。两个改动都不改对外 C ABI，不改任何数据布局。

**Tech Stack:** C++20，Vulkan 1.0 core，host ctest（无 GPU 依赖用 fake `IRenderBackend`，有 GPU
依赖用真实 `VkBackend` + lavapipe headless）。

**Spec:** `docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md`（§4a/§4c/§4d）——本
计划只实施该 spec 的 4a+4c+4d 三项，4b（swapchain 直接上屏）明确排除在外、另开一轮。

## Global Constraints

- 工作目录：`/home/qiansenwei/workspace/demo/.worktrees/A8-2`（分支 `task/A8-2`）。
- 不改 `core/interfaces/i_render_backend.h` 的 `composite()` 签名（保持 `composite(stamps,
  predicted=false)`，两次调用不合并成一次 API 调用——只改「每次 `composite()` 内部是否额外
  多付一次读回提交」，不改「`composite()` 被调用几次」）。
- `DGCPAIN_TEST_HOOKS` 新增/复用的计数字段一律 `#ifdef DGCPAIN_TEST_HOOKS` 包住，生产构建零
  开销、零新增符号（对齐既有 `dispatchCount_`/`compositeCount_`/`snapshotRefreshCount_` 模式）。
- 每个任务结束必须 `ctest --test-dir build/host-verify` 全绿才能进入下一任务（若 host-verify
  构建目录不存在或配置已过期，先 `cmake -S . -B build/host-verify -DCMAKE_BUILD_TYPE=Debug
  -DDGCPAIN_BUILD_CLI=ON -DDGCPAIN_KERNEL_BRUSH=ON -DDGCPAIN_RENDER_VULKAN=ON
  -DDGCPAIN_TEST_HOOKS=ON` 再 `cmake --build build/host-verify -j4`）。
- 真机验证（Task 5）设备是 MDP1221（`XCD1205AF826201978`），走 `adb -P 5555`（人工反向隧道，
  端口每次会话可能不同，连不上先问用户当前端口/是否需要重新建隧道，不要假设固定不变）。
- Android 侧 `./gradlew` 调用会被本环境的 context-mode 插件 hook 拦截重定向，必须用
  `mcp__plugin_context-mode_context-mode__ctx_execute`（`language: "shell"`）而非直接 Bash；
  这台构建机器常年有其他用户的并发 gradle/flutter daemon、负载重，`ctx_execute` 内部必须用
  `setsid nohup ... > logfile 2>&1 < /dev/null & disown` 把构建进程完全脱离父进程存活，
  然后用 `Monitor` 工具轮询 `logfile` 等 `BUILD SUCCESSFUL`/`BUILD FAILED`（不要用一般前台
  等待，容易被判超时杀掉进程；也不要相信"看起来卡住"，先查 `ps`/`uptime` 确认是否真的死了
  还是只是负载重）。
- 若真机需要新增诊断打印：真机固件是 `ro.build.type=user`（生产固件），native 进程 `stderr`
  不会转发进 logcat，`fprintf(stderr,...)` 在设备上完全看不到。必须用 `__android_log_print`
  （`#include <android/log.h>`，`Android.mk`/CMake 侧需 `target_link_libraries(dgc_paint
  PRIVATE log)`，本仓库 `CMakeLists.txt` 顶层已加好这行，Android 平台下自动生效）。参考
  `core/engine.cpp` 顶部已有的 `DGCPAIN_PERF_LOG` 宏（`#if defined(DGCPAIN_PERF) &&
  defined(__ANDROID__)` 时用 `__android_log_print`，否则用 `fprintf(stderr,...)`），新增插桩
  照抄这个宏，不要重新发明。
- `adb shell input swipe` 生成的触摸事件时序与真实手指画线差异巨大（已实测：合成 swipe 下
  「输入→读回」lag 50-79ms，真实手指同场景只有 6-15.5ms），**任何延迟数字的验收/对比都必须
  用真实手指操作**，合成 swipe 只能用于验证代码路径是否被触发（如确认插桩生效），不能用于
  验收延迟目标。

---

### Task 1: 修复预测批 stale-tip 顺序 bug（4c）

**Files:**
- Modify: `core/engine.h`（新增一个测试可见的自由函数声明）
- Modify: `core/engine.cpp:259-345` 附近的 `Engine::renderLoop()`/`flushBatch`/`flushAccum`
  （提取排序逻辑、修正调用顺序）
- Create: `tests/test_engine_composite_order.cpp`
- Modify: `tests/CMakeLists.txt`（注册新测试，放在 host 无条件区，仿 `test_ring_buffer`/
  `test_engine` 那两行的写法，即文件里第 19-22 行附近）

**Interfaces:**
- Consumes：`IRenderBackend`（`core/interfaces/i_render_backend.h`，已存在，签名不变）、
  `StampData`（`core/types.h`，已存在）。
- Produces：新自由函数 `void CompositeOrdered(IRenderBackend* backend,
  std::vector<StampData>* predStamps, std::vector<StampData>* realStamps)`（声明于
  `core/engine.h`，定义于 `core/engine.cpp`），供 `Engine::renderLoop()` 内部调用，也供
  `tests/test_engine_composite_order.cpp` 直接单测调用。

#### 背景（写代码前必读）

当前 `core/engine.cpp` 里 `Engine::renderLoop()` 内的 `flushAccum` lambda（约第 401-408 行）：

```cpp
auto flushAccum = [&]() {
    if (!realStamps.empty()) {
        backend_->composite(realStamps, /*predicted=*/false);
        realStamps.clear();
    }
    if (!predStamps.empty()) {
        backend_->composite(predStamps, /*predicted=*/true);
        predStamps.clear();
    }
};
```

`VkBackend::CompositeLocked()`（`render/vulkan/vk_backend.cpp` 约第 1114-1116 行）在每次
`composite()` 调用结尾都会检查：

```cpp
if (snapshotRefreshRequested_.exchange(false, std::memory_order_acq_rel)) {
    RefreshReadbackCacheLocked();
}
```

`snapshotRefreshRequested_` 是一个跨两次 `composite()` 调用共享的原子标志，`exchange` 具有
"谁先执行到这行谁就把它读到 true 并清零"的效果。由于 `flushAccum` 总是**先** composite
真实批、**后**composite 预测批，若刷新请求恰好在真实批执行完毕时为 true，`RefreshReadbackCacheLocked()`
会在**这一批的预测点还没画进 `tipImage`**之前就把画布内容拷进读回缓存——读回看到的是
「上一批」遗留的旧预测尖，不是这一批刚产生的新预测尖。这就是用户真机复测时"预测尖显示总慢
半拍"的根因。

修复：让预测批永远先于真实批 composite。这样无论刷新标志被两次 `composite()` 调用里的哪一次
消费，`tipImage`/`tipHasContent_` 在被读到时都已经反映了**这一批**的最新预测内容（若被第一次
即预测批的调用消费，`tipHasContent_` 已在同一次 `CompositeLocked()` 调用内正确置位；若被第二次
即真实批的调用消费，`tipHasContent_` 早已在前一次预测批调用里置位过，同样正确）。

- [ ] **Step 1: 在 `core/engine.h` 顶部（`#include` 之后、`class Engine` 定义之前）新增自由函数声明**

在 `core/engine.h` 里找到这段（约第 14-16 行）：

```cpp
class IPaintKernel;
class IRenderBackend;
class StrokeModeler;
```

在它之后插入：

```cpp

// bugfix-stale-tip（白盒可测）：预测批必须先于真实批 composite。VkBackend::CompositeLocked()
// 每次 composite() 结尾都会检查一个跨两次调用共享的原子刷新标志，谁先执行到检查点谁就把它
// 消费掉；若真实批先于预测批执行、又恰好消费了该标志，读回合成用的是「上一批」残留的旧
// tip（预测尖显示慢半拍）。预测批先行可保证无论标志被哪次调用消费，tipImage/tipHasContent_
// 都已反映本批最新内容。定义于 core/engine.cpp，此处声明供 Engine::renderLoop() 内部调用，
// 也供 tests/test_engine_composite_order.cpp 白盒单测（不经三线程，直接传受控 stamp 向量）。
void CompositeOrdered(IRenderBackend* backend, std::vector<StampData>* predStamps,
                      std::vector<StampData>* realStamps);
```

- [ ] **Step 2: 写失败的单测 `tests/test_engine_composite_order.cpp`**

```cpp
// bugfix-stale-tip 回归（TDD 先红后绿）：CompositeOrdered 必须先 composite 预测批、
// 后 composite 真实批——保证 VkBackend 的读回刷新标志无论被哪次调用消费，看到的
// tipImage 都已反映本批最新预测内容，不是上一批遗留的旧内容。
//
// 白盒直测：不经 Engine 三线程/SPSC 队列，直接用一个记录调用顺序的 fake IRenderBackend
// 调用 CompositeOrdered，验证三种情况（仿 tests/test_flush_throttle_engine.cpp 的
// BatchCountingBackend 写法）：
//   1) 预测批 + 真实批都非空 → 调用顺序必须是 [predicted=true, predicted=false]。
//   2) 只有真实批非空 → 只调用一次，predicted=false。
//   3) 只有预测批非空 → 只调用一次，predicted=true。
//   4) 两者皆空 → 不调用。
#include "core/engine.h"
#include "core/interfaces/i_render_backend.h"

#include <cstdio>
#include <vector>

namespace {

class OrderRecordingBackend : public IRenderBackend {
public:
    void init(PlatformSurface, int, int) override {}
    void resize(int, int) override {}
    void beginFrame() override {}
    void composite(const std::vector<StampData>&, bool predicted) override {
        order.push_back(predicted);
    }
    void clearCanvas(float, float, float, float) override {}
    void present() override {}
    void shutdown() override {}
    void initOffscreen(int, int) override {}
    void readback(void*) override {}
    void exportPNG(const char*) override {}

    std::vector<bool> order;
};

std::vector<StampData> MakeStamps(std::size_t n) {
    std::vector<StampData> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = StampData{static_cast<float>(i), 0.0f, 4.0f, 0.9f, 1.0f};
    }
    return v;
}

}  // namespace

static int failures = 0;
#define CHECK(cond, name)                                                 \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s\n", name);                     \
            ++failures;                                                   \
        }                                                                  \
    } while (0)

int main() {
    // 情况 1：预测批 + 真实批都非空 → 预测先、真实后。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred = MakeStamps(3);
        std::vector<StampData> real = MakeStamps(5);
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 2, "case1: exactly two composite() calls");
        CHECK(backend.order.size() == 2 && backend.order[0] == true,
              "case1: predicted batch composited first");
        CHECK(backend.order.size() == 2 && backend.order[1] == false,
              "case1: real batch composited second");
        CHECK(pred.empty(), "case1: predStamps cleared after composite");
        CHECK(real.empty(), "case1: realStamps cleared after composite");
    }
    // 情况 2：只有真实批非空。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred;
        std::vector<StampData> real = MakeStamps(4);
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 1 && backend.order[0] == false,
              "case2: only real composited, single call");
    }
    // 情况 3：只有预测批非空。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred = MakeStamps(2);
        std::vector<StampData> real;
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 1 && backend.order[0] == true,
              "case3: only predicted composited, single call");
    }
    // 情况 4：两者皆空 → 不调用。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred;
        std::vector<StampData> real;
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.empty(), "case4: no composite() call when both empty");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_engine_composite_order] PASS\n");
    } else {
        std::fprintf(stderr, "[test_engine_composite_order] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: 注册测试并确认先红（编译失败——`CompositeOrdered` 还不存在）**

在 `tests/CMakeLists.txt` 里找到这两行（约第 24-27 行）：

```cmake
add_executable(test_engine test_engine.cpp)
target_include_directories(test_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_engine PRIVATE dgc_paint)
add_test(NAME test_engine COMMAND test_engine)
```

在它之后插入：

```cmake

# bugfix-stale-tip：CompositeOrdered 白盒单测（无线程/无 GPU 依赖，host 无条件）。
add_executable(test_engine_composite_order test_engine_composite_order.cpp)
target_include_directories(test_engine_composite_order PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_engine_composite_order PRIVATE dgc_paint)
add_test(NAME test_engine_composite_order COMMAND test_engine_composite_order)
```

Run:
```bash
cmake -S . -B build/host-verify -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_CLI=ON \
  -DDGCPAIN_KERNEL_BRUSH=ON -DDGCPAIN_RENDER_VULKAN=ON -DDGCPAIN_TEST_HOOKS=ON
cmake --build build/host-verify --target test_engine_composite_order -j4
```
Expected: **编译失败**（`CompositeOrdered` undeclared/undefined——Step 1 只加了声明，Step 4
才加定义）。这就是本步的"红"。

- [ ] **Step 4: 在 `core/engine.cpp` 实现 `CompositeOrdered` 并接入 `flushAccum`**

在 `core/engine.cpp` 顶部匿名命名空间结束、`Engine` 类方法实现开始之前（即文件里已有的第一个
匿名命名空间 `namespace { ... }` 块的**外面**，作为一个具名的全局函数——不要放进匿名命名空间，
否则测试文件链接不到），添加：

```cpp
void CompositeOrdered(IRenderBackend* backend, std::vector<StampData>* predStamps,
                      std::vector<StampData>* realStamps) {
    if (!predStamps->empty()) {
        backend->composite(*predStamps, /*predicted=*/true);
        predStamps->clear();
    }
    if (!realStamps->empty()) {
        backend->composite(*realStamps, /*predicted=*/false);
        realStamps->clear();
    }
}
```

然后把 `Engine::renderLoop()` 里的 `flushAccum` lambda（当前先 real 后 pred 那版）替换成：

```cpp
auto flushAccum = [&]() {
    CompositeOrdered(backend_, &predStamps, &realStamps);
};
```

（`backend_` 已经是 `IRenderBackend*`，直接传，不用 `.get()`。）

- [ ] **Step 5: 确认单测转绿**

Run: `cmake --build build/host-verify --target test_engine_composite_order -j4 && ./build/host-verify/tests/test_engine_composite_order`
Expected: `[test_engine_composite_order] PASS`，退出码 0。

- [ ] **Step 6: 全量 host ctest 回归**

Run: `cmake --build build/host-verify -j4 && ctest --test-dir build/host-verify --output-on-failure`
Expected: 全绿（含既有 `test_engine`/`test_wet_tip`/`test_flush_throttle_engine`/
`test_predictor_corner_clean` 等——这一步只改了 composite 调用顺序，不改语义，行为逐位
不变，理应零回归）。

- [ ] **Step 7: 提交**

```bash
git add core/engine.h core/engine.cpp tests/test_engine_composite_order.cpp tests/CMakeLists.txt
git commit -m "fix(engine): 预测批先于真实批 composite——根治读回快照 stale-tip（预测尖显示慢半拍）

CompositeOrdered() 提取自 flushAccum，白盒单测锁定顺序；预测批先行保证
VkBackend 的 snapshotRefreshRequested_ 无论被哪次 composite() 消费，读到的
tipHasContent_/tipImage 都已反映本批最新预测内容。"
```

---

### Task 2: 新增 SubmitAndWait 计数测试钩子（为 Task 3 的 TDD 打基础）

**Files:**
- Modify: `render/vulkan/vk_backend.h`（新增 `testSubmitAndWaitCount()` 声明）
- Modify: `render/vulkan/vk_backend.cpp`（新增计数字段 + 递增 + getter 实现）
- Modify: `sdk_api/dgc_paint_c_api.cpp`（新增 `dgcTestSubmitAndWaitCount` C 访问器）

**Interfaces:**
- Consumes：无新依赖，纯给既有 `VkBackend`/`DgcContext` 加一个只读计数器。
- Produces：`std::uint64_t VkBackend::testSubmitAndWaitCount() const`（仅
  `DGCPAIN_TEST_HOOKS` 编译可见）；`extern "C" std::uint64_t dgcTestSubmitAndWaitCount(DgcContext* ctx)`
  （仅 `DGCPAIN_TEST_HOOKS && DGCPAIN_HAVE_VULKAN` 编译可见），供 Task 3 的回归测试与未来任何
  "GPU 提交次数"相关测试复用。

本任务纯基础设施、无独立 TDD 红绿（计数器本身没有"正确/错误"之分，只是新增可观测量），跳过
写"失败测试"步骤，直接实现 + 用 Task 3 的测试验证其确实可用。

- [ ] **Step 1: `render/vulkan/vk_backend.cpp` 里给 `Impl` 加计数字段**

找到（约第 449-450 行）：

```cpp
    std::uint64_t snapshotRefreshCount_ = 0;
    std::uint64_t compositeCount_ = 0;
#endif
```

改成：

```cpp
    std::uint64_t snapshotRefreshCount_ = 0;
    std::uint64_t compositeCount_ = 0;
    // 4a 回归：SubmitAndWait() 实际调用次数（每次 GPU 提交+等 fence +1）。修复前每次
    // composite-with-refresh 会付两次（composite 自己一次 + RefreshReadbackCacheLocked
    // 自己一次）；修复后合并成一次（composite 触发的刷新不再额外付提交，仅 Clear*/
    // flushReadbackCache 独立调用点仍各付一次）。
    std::uint64_t submitAndWaitCount_ = 0;
#endif
```

- [ ] **Step 2: 在 `SubmitAndWait()` 里递增**

找到（约第 930-945 行）`void SubmitAndWait() { ... }` 的函数体开头，在
`if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {` 这行**之前**插入：

```cpp
#ifdef DGCPAIN_TEST_HOOKS
        ++submitAndWaitCount_;
#endif
```

- [ ] **Step 3: 新增 getter 实现**

找到（约第 1449-1452 行）：

```cpp
std::uint64_t VkBackend::testCompositeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->compositeCount_;
}
#endif
```

改成：

```cpp
std::uint64_t VkBackend::testCompositeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->compositeCount_;
}

std::uint64_t VkBackend::testSubmitAndWaitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->submitAndWaitCount_;
}
#endif
```

- [ ] **Step 4: `render/vulkan/vk_backend.h` 新增声明**

找到（约第 67-71 行）：

```cpp
    std::uint64_t testSnapshotRefreshCount() const;
    std::uint64_t testCompositeCount() const;
#endif
```

改成：

```cpp
    std::uint64_t testSnapshotRefreshCount() const;
    std::uint64_t testCompositeCount() const;
    // 4a 回归：GPU 提交+等 fence 的实际调用次数，供 test_snapshot_refresh_throttle
    // 断言「合并提交后不再比 compositeCount 多付太多次」。
    std::uint64_t testSubmitAndWaitCount() const;
#endif
```

- [ ] **Step 5: `sdk_api/dgc_paint_c_api.cpp` 新增 C 访问器**

找到（约第 596-602 行）：

```cpp
std::uint64_t dgcTestCompositeCount(DgcContext* ctx) {
    if (ctx == nullptr || !ctx->impl_) {
        return 0;
    }
    auto* vk = dynamic_cast<VkBackend*>(ctx->impl_->backend.get());
    return vk ? vk->testCompositeCount() : 0;
}
```

在它之后插入：

```cpp

std::uint64_t dgcTestSubmitAndWaitCount(DgcContext* ctx) {
    if (ctx == nullptr || !ctx->impl_) {
        return 0;
    }
    auto* vk = dynamic_cast<VkBackend*>(ctx->impl_->backend.get());
    return vk ? vk->testSubmitAndWaitCount() : 0;
}
```

- [ ] **Step 6: 编译确认无回归（暂无新用例，只确认能编译链接）**

Run: `cmake --build build/host-verify -j4`
Expected: 编译成功，无告警（新增符号仅 `DGCPAIN_TEST_HOOKS` 下可见，host-verify 预设已开）。

- [ ] **Step 7: 提交**

```bash
git add render/vulkan/vk_backend.h render/vulkan/vk_backend.cpp sdk_api/dgc_paint_c_api.cpp
git commit -m "test(vk_backend): 新增 SubmitAndWait 调用计数钩子——为 4a 合并提交回归打基础"
```

---

### Task 3: 合并 composite + 读回刷新为一次 GPU 提交（4a）

**Files:**
- Modify: `render/vulkan/vk_backend.cpp`（拆分 `RefreshReadbackCacheLocked`，接入
  `CompositeLocked`）
- Modify: `tests/test_snapshot_refresh_throttle.cpp`（新增合并提交断言）

**Interfaces:**
- Consumes：Task 2 产出的 `testSubmitAndWaitCount()`/`dgcTestSubmitAndWaitCount`。
- Produces：无新公开接口——`RefreshReadbackCacheLocked()` 对外行为（谁调用、何时刷新）
  完全不变，只是内部实现拆成了两个私有辅助方法 `RecordRefreshCommands()`/
  `FinishRefreshReadback()`。

#### 背景（写代码前必读）

当前 `CompositeLocked()` 结尾（约第 1083-1116 行）：

```cpp
        SubmitAndWait();
        // ...PERF/RenderDoc 宏块，不动...
        if (predicted) {
            tipHasContent_ = true;
        }
        // ...注释...
        if (snapshotRefreshRequested_.exchange(false, std::memory_order_acq_rel)) {
            RefreshReadbackCacheLocked();
        }
    }
```

`RefreshReadbackCacheLocked()`（约第 1229-1291 行）自己重新 `BeginCommands()`（会
`vkResetCommandBuffer` 把 `CompositeLocked()` 刚提交完的 command buffer 清空重录），意味着
一次「composite 批 + 顺带刷新」实际付了**两次独立** `vkQueueSubmit`+`vkWaitForFences`
往返。真机 Mali 实测每次这样的独立提交固定开销约 2-3ms（详见
`docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md` §1）。

修复：把 `RefreshReadbackCacheLocked()` 拆成两半——GPU 命令录制部分（merge dispatch 条件性 +
`vkCmdCopyImageToBuffer`）不再自己开/提交 command buffer，改由调用方决定何时录制、何时提交；
`CompositeLocked()` 在自己**已经打开**的 command buffer 里、`SubmitAndWait()` 之前，若命中
刷新标志就顺带录进去，只在结尾统一 `SubmitAndWait()` 一次。

**关键顺序陷阱**：现在 `tipHasContent_ = true`（第 1099-1101 行）发生在 `SubmitAndWait()`
**之后**；而合并后刷新命令录制必须发生在 `SubmitAndWait()` **之前**。若不调整，`predicted`
批自己触发的刷新会读到**旧的** `tipHasContent_`（还没被这一批置真），重新引入 Task 1 刚修的
stale-tip 问题的一个变体。必须把 `if (predicted) tipHasContent_ = true;` 一并挪到
`SubmitAndWait()` 之前、刷新判断之前。

- [ ] **Step 1: 拆分 `RefreshReadbackCacheLocked()` 为两个辅助方法**

把现有（约第 1229-1291 行）整个函数体：

```cpp
    void RefreshReadbackCacheLocked() {
        if (!canvasReady) {
            return;
        }
#ifdef DGCPAIN_TEST_HOOKS
        ++snapshotRefreshCount_;  // 每次实际快照拷贝 +1（test hook，仅测试构建）。
#endif
        BeginCommands();
        // A8-2：有 tip 时先 fullscreen merge（canvas+tip → displayImage），读回源改为
        // displayImage；无 tip 时读回源仍为 canvasImage（与改造前逐位一致、零额外 GPU 拷贝）。
        VkImage srcImage = canvasImage.get();
        if (tipHasContent_) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    mergePipelineLayout, 0, 1, &mergeDescriptorSet, 0, nullptr);
            vkCmdDispatch(commandBuffer, (uint32_t)((width + 7) / 8),
                          (uint32_t)((height + 7) / 8), 1);
            // merge 写 displayImage（shader write）→ CopyImageToBuffer 读（transfer read）。
            VkImageMemoryBarrier mergeBarrier{};
            mergeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            mergeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mergeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            mergeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            mergeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            mergeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mergeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mergeBarrier.image = displayImage;
            mergeBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &mergeBarrier);
            srcImage = displayImage.get();
        }
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, srcImage, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer,
                               1, &region);
        SubmitAndWait();
        void* data = nullptr;
        vkMapMemory(device, readbackMemory, 0, readbackSize, 0, &data);
        if (readbackCached_) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readbackMemory.get();
            range.offset = 0;
            range.size = readbackSize;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.resize((size_t)readbackSize);
            std::memcpy(cache_.data(), data, (size_t)readbackSize);
        }
        vkUnmapMemory(device, readbackMemory);
    }
```

替换成三个函数：

```cpp
    // 4a：录制读回刷新的 GPU 命令（merge dispatch 条件性 + copy-to-buffer）到**当前已打开**
    // 的 command buffer；调用方必须已调过 BeginCommands()，本函数不调 SubmitAndWait()——
    // 可以是独立一次提交的一部分（RefreshReadbackCacheLocked 单独调用场景），也可以是
    // CompositeLocked 自己那次提交的一部分（合并省一次 GPU 往返）。
    void RecordRefreshCommands() {
#ifdef DGCPAIN_TEST_HOOKS
        ++snapshotRefreshCount_;  // 每次实际快照刷新 +1（test hook，仅测试构建）。
#endif
        // A8-2：有 tip 时先 fullscreen merge（canvas+tip → displayImage），读回源改为
        // displayImage；无 tip 时读回源仍为 canvasImage（与改造前逐位一致、零额外 GPU 拷贝）。
        VkImage srcImage = canvasImage.get();
        if (tipHasContent_) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    mergePipelineLayout, 0, 1, &mergeDescriptorSet, 0, nullptr);
            vkCmdDispatch(commandBuffer, (uint32_t)((width + 7) / 8),
                          (uint32_t)((height + 7) / 8), 1);
            VkImageMemoryBarrier mergeBarrier{};
            mergeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            mergeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mergeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            mergeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            mergeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            mergeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mergeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mergeBarrier.image = displayImage;
            mergeBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &mergeBarrier);
            srcImage = displayImage.get();
        }
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, srcImage, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer,
                               1, &region);
    }

    // 4a：读回刷新的提交后 CPU 处理（map/invalidate/memcpy/unmap 进 cache_）。调用方必须
    // 已在 RecordRefreshCommands() 之后调过 SubmitAndWait()（即 GPU 已完成拷贝）。
    void FinishRefreshReadback() {
        void* data = nullptr;
        vkMapMemory(device, readbackMemory, 0, readbackSize, 0, &data);
        if (readbackCached_) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readbackMemory.get();
            range.offset = 0;
            range.size = readbackSize;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.resize((size_t)readbackSize);
            std::memcpy(cache_.data(), data, (size_t)readbackSize);
        }
        vkUnmapMemory(device, readbackMemory);
    }

    // 独立调用点（ClearCanvasLocked/ClearTipLocked）保持原有语义：自己开一次完整的
    // Begin→Record→Submit→Finish，不与任何 composite 合并（这两处频率低，不是本次优化目标）。
    void RefreshReadbackCacheLocked() {
        if (!canvasReady) {
            return;
        }
        BeginCommands();
        RecordRefreshCommands();
        SubmitAndWait();
        FinishRefreshReadback();
    }
```

- [ ] **Step 2: 改 `CompositeLocked()`，把 `tipHasContent_` 置位与刷新录制挪到 `SubmitAndWait()` 之前**

找到（约第 1083-1116 行）：

```cpp
        SubmitAndWait();
#ifdef DGCPAIN_PERF
        auto t1 = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "[PERF] composite stamps=%zu dispatches=%zu total=%.3f ms (%dx%d)\n",
                     stamps.size(), dispatches,
                     std::chrono::duration<double, std::milli>(t1 - t0).count(), width, height);
#endif
#ifdef DGCPAIN_RENDERDOC_ENABLED
        if (rdc) {
            rdc->endFrameCapture(rdocDevice);
            initCaptureOpen_ = false;
        }
#endif
        // A8-2：预测批提交完成后置位 tipHasContent_，读回快照才会走 merge 路径。
        // 真实批不置位（tip 内容不变）。
        if (predicted) {
            tipHasContent_ = true;
        }
        // bugfix（20fps 回退）：composite 批提交完成后顺手发布快照，供 readback() 直接
        // memcpy——放在 RenderDoc capture 窗口结束之后，不把这次内部拷贝计入抓帧。
        //
        // Bug #3（快照刷新节流）：不再**无条件**刷新——连续绘制时渲染线程每 ≤4ms
        // （kMaxBatchDurationMs overCap）composite 一次，若每次都付全画布 GPU→CPU 拷贝
        // （Mali 上数 ms），弱 GPU 被打饱和 → Android 60→30 掉帧（PC 桌面 GPU 拷贝 ~1ms
        // 不掉帧，解释「PC 没这个问题」）。改为：仅当消费者请求过（requestSnapshotRefresh()
        // 置位，见 engine.cpp 的 requestFlush/flush/renderLoop 排空联动）才在此刷新；
        // overCap 自动合批的 composite 不再付拷贝。exchange 原子清位，一次请求结算一次
        // 刷新（同批多次请求折叠为一次，语义不变）。确定性不变：内容仍由渲染线程在完整
        // 批提交后发布 → 读回永远读到「完整画布」（≤1 批滞后）；drain 请求强制刷新 →
        // 精确像素路径不变。
        if (snapshotRefreshRequested_.exchange(false, std::memory_order_acq_rel)) {
            RefreshReadbackCacheLocked();
        }
    }
```

替换成：

```cpp
        // A8-2：预测批 dispatch 完成后置位 tipHasContent_（挪到 SubmitAndWait() 之前，
        // 4a 合并提交需要——若本批同时命中刷新标志，下面 RecordRefreshCommands() 得读到
        // 「这一批」刚置好的 tipHasContent_，不能是 SubmitAndWait 之后才置）。真实批不
        // 置位（tip 内容不变）。
        if (predicted) {
            tipHasContent_ = true;
        }
        // 4a：把读回刷新（merge+copy）合进这次 composite 已经打开的 command buffer，
        // 只 SubmitAndWait() 一次，不再像修复前那样「composite 自己提交一次 + 刷新
        // 又单独提交一次」（真机 Mali 实测每次独立提交固定开销约 2-3ms，见
        // docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md §1/§4a）。
        // Bug #3（快照刷新节流）语义不变：仍是「仅当消费者请求过才刷新」，只是刷新的
        // GPU 提交现在跟 dab 合成共享同一次 submit。
        const bool doRefresh =
            snapshotRefreshRequested_.exchange(false, std::memory_order_acq_rel);
        if (doRefresh) {
            RecordRefreshCommands();
        }
        SubmitAndWait();
#ifdef DGCPAIN_PERF
        auto t1 = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "[PERF] composite stamps=%zu dispatches=%zu total=%.3f ms (%dx%d)\n",
                     stamps.size(), dispatches,
                     std::chrono::duration<double, std::milli>(t1 - t0).count(), width, height);
#endif
#ifdef DGCPAIN_RENDERDOC_ENABLED
        if (rdc) {
            rdc->endFrameCapture(rdocDevice);
            initCaptureOpen_ = false;
        }
#endif
        if (doRefresh) {
            FinishRefreshReadback();
        }
    }
```

- [ ] **Step 3: 全量 host ctest 回归（先验证零回归，这一步应该直接绿——本任务不改任何
      对外可观测行为，只改 GPU 提交次数）**

Run: `cmake --build build/host-verify -j4 && ctest --test-dir build/host-verify --output-on-failure`
Expected: 全绿，尤其 `test_wet_tip`/`test_readback_drain`/`test_midstroke_readback`/
`test_perf_regression`/`test_determinism`/`test_snapshot_refresh_throttle` 逐位行为不变。

- [ ] **Step 4: 给 `test_snapshot_refresh_throttle.cpp` 加新断言（先红后绿验证合并确实生效）**

找到该文件末尾（约第 158-172 行）：

```cpp
    // ── 节流断言（本 bug 先红后绿核心）──
    const std::uint64_t refresh = dgcTestSnapshotRefreshCount(ctx);
    const std::uint64_t composite = dgcTestCompositeCount(ctx);
    std::fprintf(stderr,
                 "[test_snapshot_refresh_throttle] snapshotRefreshCount=%llu compositeCount=%llu "
                 "ratio=%.3f\n",
                 (unsigned long long)refresh, (unsigned long long)composite,
                 composite > 0 ? (double)refresh / (double)composite : 0.0);
    CHECK(composite > 0, "composite batches happened (overCap triggered)");
    CHECK(refresh > 0, "snapshot refresh happened (clear + settle + drain)");
    // 确定性上界：刷新只发生在 clear + 队列排空结算 + drain 请求，不随 composite 数量波动。
    // 修复前 refresh ≈ composite ≥ 13 必然超界（红）；修复后 refresh ∈ [2,6] 必然通过（绿）。
    CHECK(refresh <= kMaxRefreshBound,
          "snapshot refresh throttled: refresh <= kMaxRefreshBound "
          "(was ~composite before fix)");

    dgcDestroy(ctx);
```

改成（新增 `submitAndWait` 读取 + 断言，插在 `dgcDestroy(ctx)` 之前）：

```cpp
    // ── 节流断言（本 bug 先红后绿核心）──
    const std::uint64_t refresh = dgcTestSnapshotRefreshCount(ctx);
    const std::uint64_t composite = dgcTestCompositeCount(ctx);
    std::fprintf(stderr,
                 "[test_snapshot_refresh_throttle] snapshotRefreshCount=%llu compositeCount=%llu "
                 "ratio=%.3f\n",
                 (unsigned long long)refresh, (unsigned long long)composite,
                 composite > 0 ? (double)refresh / (double)composite : 0.0);
    CHECK(composite > 0, "composite batches happened (overCap triggered)");
    CHECK(refresh > 0, "snapshot refresh happened (clear + settle + drain)");
    // 确定性上界：刷新只发生在 clear + 队列排空结算 + drain 请求，不随 composite 数量波动。
    // 修复前 refresh ≈ composite ≥ 13 必然超界（红）；修复后 refresh ∈ [2,6] 必然通过（绿）。
    CHECK(refresh <= kMaxRefreshBound,
          "snapshot refresh throttled: refresh <= kMaxRefreshBound "
          "(was ~composite before fix)");

    // 4a 合并提交断言（先红后绿）：composite 触发的刷新（settle 那部分）不应再各自
    // 额外付一次 GPU 提交——submitAndWaitCount 应约等于 compositeCount，只比它多出
    // 「非 composite 触发」的刷新次数（本场景固定是 clear(1) + drain 收尾(1) = 2，
    // 这两处仍各自独立 Begin/Submit，本任务不改）。
    // 修复前：每次刷新（不论 composite 触发还是 clear/drain 触发）都各自额外付一次提交
    //   → submitAndWait ≈ composite + refresh，refresh 最多到 kMaxRefreshBound(10)，
    //   即 submitAndWait 可能到 composite+10，必然超过 composite+2（红）。
    // 修复后：composite 触发的刷新合并进 composite 自己那次提交，不再额外计数
    //   → submitAndWait = composite + 2（clear + drain 各一次）（绿）。
    const std::uint64_t submitAndWait = dgcTestSubmitAndWaitCount(ctx);
    std::fprintf(stderr,
                 "[test_snapshot_refresh_throttle] submitAndWaitCount=%llu "
                 "(composite+2 bound=%llu)\n",
                 (unsigned long long)submitAndWait, (unsigned long long)(composite + 2));
    CHECK(submitAndWait <= composite + 2,
          "4a: composite-triggered refresh no longer costs an extra GPU submit "
          "(submitAndWaitCount <= compositeCount + 2)");

    dgcDestroy(ctx);
```

在文件顶部的 `extern "C"` 声明块（约第 38-42 行）里加一行：

```cpp
#ifdef DGCPAIN_TEST_HOOKS
// 测试访问器不在公开头（dgc_paint_c_api.h）中（仅测试编译进库），此处自行声明。
extern "C" std::uint64_t dgcTestSnapshotRefreshCount(DgcContext* ctx);
extern "C" std::uint64_t dgcTestCompositeCount(DgcContext* ctx);
extern "C" std::uint64_t dgcTestSubmitAndWaitCount(DgcContext* ctx);
#endif
```

- [ ] **Step 5: 编译运行，确认新断言绿（本任务代码已在 Step 1-2 落地，理应直接绿——
      若红说明 Step 1-2 的改动有遗漏，回去检查）**

Run:
```bash
cmake --build build/host-verify --target test_snapshot_refresh_throttle -j4
./build/host-verify/tests/test_snapshot_refresh_throttle
```
Expected: `[test_snapshot_refresh_throttle] PASS`，日志里 `submitAndWaitCount` 那行的值
`<= composite+2`。

- [ ] **Step 6: 全量 host ctest 回归 + 泄漏检查**

Run:
```bash
ctest --test-dir build/host-verify --output-on-failure
cmake -S . -B build/host-sanitize-verify -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_SANITIZE=ON \
  -DDGCPAIN_BUILD_CLI=ON -DDGCPAIN_KERNEL_BRUSH=ON -DDGCPAIN_RENDER_VULKAN=ON \
  -DDGCPAIN_TEST_HOOKS=ON
cmake --build build/host-sanitize-verify -j4
ctest --test-dir build/host-sanitize-verify --output-on-failure
```
Expected: 两套构建全绿，ASan/LSan 零泄漏（本任务只重排了函数调用顺序，未新增任何堆分配/
资源持有，理应零风险，但仍需实际跑一遍确认）。

- [ ] **Step 7: 提交**

```bash
git add render/vulkan/vk_backend.h render/vulkan/vk_backend.cpp tests/test_snapshot_refresh_throttle.cpp
git commit -m "perf(vk_backend): 合并 composite 与读回刷新为一次 GPU 提交（4a）

RefreshReadbackCacheLocked 拆分为 RecordRefreshCommands（GPU 命令录制）+
FinishRefreshReadback（提交后 CPU 处理）；CompositeLocked 在自己的
SubmitAndWait 之前顺带录制刷新命令，省掉一次独立 GPU 提交往返（真机 Mali
实测每次固定开销约 2-3ms）。ClearCanvasLocked/ClearTipLocked/
flushReadbackCache 三个独立调用点行为不变。

新增 submitAndWaitCount 测试钩子 + test_snapshot_refresh_throttle 断言
（submitAndWaitCount <= compositeCount+2）先红后绿验证合并生效。"
```

---

### Task 4: 提交 paint-android 消费端待落地的预测参数改动

**Files（另一个仓库，非本 worktree）：**
- Modify: `/home/qiansenwei/workspace/paint-android/app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`
  （工作区已有未提交改动：`wobble_timeout_ms` 默认 10、预测开关 interval 20ms、启动时下发
  全部 modeler 默认参数——本任务只是把这些已验证有效的改动提交，不再新写代码）

本任务无 TDD 步骤（改动已在之前的会话里手工验证过），只是收尾提交，供 Task 5 真机验证时
使用一致的、已提交的基线（避免真机测试时消费端还是脏工作区，结果无法复现/追溯）。

- [ ] **Step 1: 确认改动仍在且内容符合预期**

Run: `cd /home/qiansenwei/workspace/paint-android && git status --short && git diff app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt`

Expected 输出里应该能看到：
- `BrushSettingSpec(4, "抖动消除超时 wobble_timeout_ms", 0f, 200f, 10f, ...)` （默认值从 40
  改成 10）
- `val next = if (predictionOn) 0f else 20f` （原来是 30f）
- 新增一个 `LaunchedEffect(started) { if (started) { BRUSH_SETTINGS.forEach { spec -> if
  (spec.id >= 4) ctx.nativeSetBrushSetting(spec.id, spec.default.toDouble()) } } }`
- 新增 `drawLagProbe`/`Ref<T>` 相关的延迟探针改动（本次会话诊断用，予以保留——它只是加了
  HUD 显示，不影响行为）

若以上任一项缺失，先补回（本文档不重复给出这些具体代码，均已在当前工作区文件里，直接
`git diff` 核对，不要凭空重写）。

- [ ] **Step 2: 提交**

```bash
cd /home/qiansenwei/workspace/paint-android
git add app/src/main/java/com/dgcamp/paint/ui/PaintScreen.kt
git commit -m "feat(paint-screen): wobble_timeout 默认改 10ms + 预测 interval 改 20ms + 启动下发 modeler 默认参数

真机复测发现 wobble_timeout=40ms 是不跟手主因（真机 40→10 后画布滞后
71px→26px @1500px/s）；SDK modeler 惰性激活此前从不下发消费端默认值，
导致预测开关首次点按时用的是 SDK 内置 40ms 而非这里配的值，启动即下发
修复这个落差。新增输入→上屏延迟探针（drawLagProbe）用于诊断真实端到端
延迟构成，与「输入→读回」探针对照。"
```

- [ ] **Step 3: 推送**

Run: `git push origin a8-2-preverify`

---

### Task 5: 真机重新验证（4d，验收判定，非代码任务）

本任务是决策点，不是固定代码改动——按 spec §4d，产出是"是否达标"的结论，不是新功能。

**Files:** 无代码修改（除非验证不达标、需要记录后续任务，见 Step 5）。

- [ ] **Step 1: 把 Task 1+3 的 SDK 改动同步进 paint-android 的 sdk submodule**

```bash
cd /home/qiansenwei/workspace/demo/.worktrees/A8-2
git push origin task/A8-2
cd /home/qiansenwei/workspace/paint-android/sdk
git fetch origin task/A8-2
git checkout <Task 3 Step 7 提交的 commit hash>
cd /home/qiansenwei/workspace/paint-android
git add sdk
git commit -m "chore(sdk): 前移 submodule——4a 合并 GPU 提交 + 4c stale-tip 顺序修复"
git push origin a8-2-preverify
```

- [ ] **Step 2: 清空原生构建缓存并重新构建（cppFlags 未变，但 native 源码变了，保险起见
      清缓存避免增量构建遗漏）**

用 `mcp__plugin_context-mode_context-mode__ctx_execute`（不要直接 Bash，会被 hook 拦截）：

```bash
cd /home/qiansenwei/workspace/paint-android && rm -rf app/.cxx && rm -f /tmp/gradle_task5.log && setsid nohup ./gradlew assembleDebug --console=plain > /tmp/gradle_task5.log 2>&1 < /dev/null &
disown
echo "LAUNCHED"
```

然后用 `Monitor` 工具轮询 `/tmp/gradle_task5.log` 直到出现 `BUILD SUCCESSFUL`/`BUILD FAILED`
（命令：`until grep -qE "BUILD SUCCESSFUL|BUILD FAILED" /tmp/gradle_task5.log 2>/dev/null; do
sleep 5; done; echo DONE; tail -15 /tmp/gradle_task5.log`）。这台机器负载重，可能需要几分钟到
十几分钟，属正常现象，不要中途判定"卡住"就重新发起构建（会平白多出竞争的 daemon）。

- [ ] **Step 3: 安装到设备，确认设备连接**

```bash
adb -P 5555 devices
```
若看不到 `XCD1205AF826201978`（或不同于此的当前 serial），先问用户当前反向隧道端口/设备
serial 是否变了，不要凭空假设固定值。

```bash
adb -P 5555 -s XCD1205AF826201978 install -r app/build/outputs/apk/debug/app-debug.apk
```

- [ ] **Step 4: 真机手动测试并记录数据**

请用户（或若有 accessibility/自动化手段，人工操作更可靠——本步骤禁止用 `adb shell input
swipe` 代替真实手指，之前已证实合成 swipe 时序与真实手指差异巨大、不能用于延迟数字验收）：

1. 打开 app，确认渲染模式是 SDK（不是 INK）。
2. 点开「预测」开关（切到"预测：开"状态）。
3. 用真实手指快速挥摆画一笔，目标速度 ~600mm/s（即约 2 秒内画过大半个屏幕宽度那种"甩线"
   手感，不是慢慢描）。
4. 记录 HUD 上的「输入→读回 lag」「输入→上屏 lag」两个数字（应该比 Task 3 之前的
   ~6ms/~15.5ms 低——具体降多少取决于 4a 省掉的那次提交在真机上的真实占比，参考值：
   若 4a 生效，预期读回 lag 降到 ~3-4ms 量级）。
5. 肉眼观察：画完这一笔后，墨迹落后手指指尖的距离，用手指宽度或屏幕上可见的参照物
   （比如画布网格/UI 元素间距）估算大致的 mm 数（没有精确测量工具就用"目测像素数"
   换算——1 canvas px ≈ 0.1058mm，见 spec §1 换算公式）。
6. 关闭「预测」开关，用同样的手速再画一笔，同样记录落后距离，供开/关对照。

- [ ] **Step 5: 判定与记录结论**

若 Step 4 测得的「预测开」状态下总落后距离 **≤3mm**：

```bash
cd /home/qiansenwei/workspace/demo/.worktrees/A8-2
```

在 `docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md` 末尾追加一个"§7 4d 验证
结论"小节，写入实测的 lag 数字与落后距离，注明"已达标，预测模型（弦方向外推+
interval=20ms/wobble=10ms）无需再改，4b（swapchain）作为独立任务视需要另行评估"，提交：

```bash
git add docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md
git commit -m "docs(spec): 补充 4d 真机验证结论——已达标 ≤3mm，预测模型无需再改"
git push origin task/A8-2
```

若 **未达标**（落后距离仍明显大于 3mm）：同样追加"§7 4d 验证结论"小节，如实记录实测数字，
注明"未达标，根因待查（可能是 4a 的收益不及预期，或模型器自身滞后比预估的更大，或还有
未发现的延迟源），曲率感知预测模型或进一步管线优化留作后续独立任务评估，本计划到此为止
不在本轮继续深挖"，同样提交推送。**不要在本任务范围内继续设计新方案**——按 spec 的
非目标声明，那是另一轮 brainstorm 的工作。

---

## 任务顺序与依赖

Task 1 → Task 2 → Task 3 → Task 4 → Task 5（严格顺序，每个任务的 ctest 全绿是进入下一个的
前提；Task 4/5 涉及另一个仓库和真机，不能跳过前面 host 验证直接跑真机）。

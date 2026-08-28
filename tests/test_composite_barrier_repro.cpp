// Bug3（dab 合成孔洞）回归用例（先红后绿）。
//
// 根因：CompositeLocked() 循环里每个 dab 各发一次 vkCmdPushConstants + vkCmdDispatch，
// 两次 dispatch 之间没有插 vkCmdPipelineBarrier。相邻 dab 包围盒/画布覆盖区域几乎必然
// 重叠（间距 2.5px << 半径 10px），dispatch N+1 的 shader 对同一批像素 imageLoad 读
// dispatch N 刚写的值再 imageStore 写回。Vulkan 规范下同一 command buffer 内连续两次
// dispatch 读写同一张 storage image 的重叠区域，没有显式 barrier 时驱动不保证可见性
// —— 未定义行为，部分驱动乱序/并行调度时会读到脏值，表现成局部覆盖丢失（孔洞，用户
// 在 Windows 真机 RenderDoc 抓帧截图 a-capture_16.rdc 复现）。
//
// 本测试做两件事：
//  1. 行为验证（本 bug 真正的先红后绿）：经 DGCPAIN_TEST_HOOKS 编译进库的 dispatch/barrier
//     计数器，断言「barrierCount == dispatchCount == 5」——每次 dispatch 后都必须有同步 barrier。
//     红：未加 barrier 时 barrierCount==0、dispatchCount==5 → 断言失败。
//     绿：CompositeLocked 每次 dispatch 后插入 barrier → 两者相等，断言通过。
//  2. 覆盖不变式（本机本来该绿，用于确认加 barrier 不破坏正确性）：把从 RenderDoc 抓帧
//     a-capture_16.rdc 解出的真实 5-dab 数据（登记为测试内常量数组，不依赖外部 csv/rdc 文件）
//     喂给生产代码 VkBackend::composite()，断言 dab 覆盖的椭圆区域内所有像素 alpha>0
//     （数据本身保证的完全覆盖不因加 barrier 而破坏）。落一张 PNG 到
//     /tmp/test_composite_barrier_repro.png 供人工复核。
//
// 验证边界（如实标注）：本 CI 机是软件 Vulkan（lavapipe），本来不会自然触发孔洞——本测试
// 验证的是「barrier 确实被调用、且不破坏正确性」；孔洞现象本身的最终确认需要用户在原设备
// （Windows 真机）复测（计划文档 ⑤.4，外部依赖）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/types.h"
#include "render/vulkan/vk_backend.h"

static int failures = 0;
#define CHECK(cond, name)                              \
    do {                                               \
        if (!(cond)) {                                 \
            std::fprintf(stderr, "FAIL: %s\n", name);  \
            ++failures;                                \
        }                                              \
    } while (0)

namespace {

// 从 RenderDoc 抓帧 a-capture_16.rdc 解出的真实 5-dab 数据。y 全部 798.0，x 依次
// 826.245/823.745/821.245/818.745/816.245（间距 2.5px），radius=10, hardness=0.7,
// softness=0, opacity=1, rgb=(0,0,0)。StampData 成员序 = {x,y,radius,hardness,opacity,r,g,b,softness}。
constexpr int kNumDabs = 5;
const StampData kRealDabs[kNumDabs] = {
    StampData{826.245f, 798.0f, 10.0f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    StampData{823.745f, 798.0f, 10.0f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    StampData{821.245f, 798.0f, 10.0f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    StampData{818.745f, 798.0f, 10.0f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    StampData{816.245f, 798.0f, 10.0f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
};

// 画布尺寸留边距，把 5 个 dab 平移/缩放到局部坐标（左上角留边距），避免为对齐原始画布
// 坐标分配一张几千像素的大图（同 diag_hole_repro 的做法）。
constexpr int kW = 48;
constexpr int kH = 32;
constexpr float kShiftX = 24.0f - 821.245f;  // 画布中心 x(24) - 5 个 dab 的 x 均值
constexpr float kShiftY = 16.0f - 798.0f;    // 画布中心 y(16) - dab 的 y

std::vector<StampData> LocalStamps() {
    std::vector<StampData> out;
    out.reserve(kNumDabs);
    for (const StampData& s : kRealDabs) {
        StampData t = s;
        t.x = s.x + kShiftX;
        t.y = s.y + kShiftY;
        out.push_back(t);
    }
    return out;
}

// 像素中心到任一 dab 圆心的最小归一化距离（dist = |p - c| / radius，0=圆心，1=半径边缘）。
float MinDist(const std::vector<StampData>& stamps, float px, float py) {
    float m = 1e9f;
    for (const StampData& s : stamps) {
        const float dx = px - s.x;
        const float dy = py - s.y;
        m = std::min(m, std::sqrt(dx * dx + dy * dy) / std::max(s.radius, 1e-3f));
    }
    return m;
}

}  // namespace

int main() {
    const std::vector<StampData> stamps = LocalStamps();

    // ── 覆盖不变式 + PNG 落盘 ──
    // 透明底（alpha 是「是否被 dab 覆盖」的直接判据）：coverage>=1e-3 的像素 alpha 必须 >0。
    // 用同一个实例再在透明白底上重画一遍导出 PNG，供肉眼复核「孔洞=黑色笔迹中的白点」。
    {
        VkBackend backend;
        backend.initOffscreen(kW, kH);

        backend.clearCanvas(0.0f, 0.0f, 0.0f, 0.0f);  // 透明底
        backend.composite(stamps);
        std::vector<std::uint8_t> rgba((size_t)kW * kH * 4, 0);
        backend.readback(rgba.data());

        // 覆盖不变式：像素中心距任一 dab 圆心 dist < 0.85（椭圆内侧，远离边缘/AA 衰减带，
        // coverage 必然 >0，alpha 必然 >0）。任何 alpha==0 即覆盖丢失（孔洞）。
        int coveredPixels = 0;
        int holePixels = 0;
        for (int py = 0; py < kH; ++py) {
            for (int px = 0; px < kW; ++px) {
                if (MinDist(stamps, (float)px + 0.5f, (float)py + 0.5f) < 0.85f) {
                    ++coveredPixels;
                    const std::uint8_t a = rgba[((size_t)py * kW + (size_t)px) * 4 + 3];
                    if (a == 0) {
                        ++holePixels;
                    }
                }
            }
        }
        std::fprintf(stderr, "[test_composite_barrier_repro] covered=%d hole(alpha==0)=%d\n",
                     coveredPixels, holePixels);
        CHECK(coveredPixels > 0, "coverage region non-empty (dabs inside canvas)");
        CHECK(holePixels == 0, "no hole: alpha>0 across dab ellipse coverage area");

        // 白底重画一遍，导出 PNG 供人工复核（孔洞在白色底上呈黑色笔迹中的白点）。
        backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);
        backend.composite(stamps);
        backend.exportPNG("/tmp/test_composite_barrier_repro.png");
        backend.shutdown();
    }

    // ── 行为验证（本 bug 的先红后绿核心）：barrierCount == dispatchCount == 5 ──
    // 独立实例：计数从 0 起，一次 composite(5 个 dab) 后 dispatch 恰为 5。
    {
        VkBackend backend;
        backend.initOffscreen(kW, kH);
        backend.clearCanvas(0.0f, 0.0f, 0.0f, 0.0f);
        backend.composite(stamps);
        const std::uint64_t dispatchCount = backend.testDispatchCount();
        const std::uint64_t barrierCount = backend.testBarrierCount();
        backend.shutdown();

        std::fprintf(stderr,
                     "[test_composite_barrier_repro] dispatchCount=%llu barrierCount=%llu\n",
                     (unsigned long long)dispatchCount, (unsigned long long)barrierCount);
        CHECK(dispatchCount == kNumDabs, "dispatchCount == 5 (all 5 dabs dispatched)");
        CHECK(barrierCount == dispatchCount,
              "barrierCount == dispatchCount (every dispatch followed by a barrier)");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_composite_barrier_repro] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}

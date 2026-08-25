#pragma once

#include <cstdint>
#include <memory>

#include "core/interfaces/i_paint_kernel.h"

// BrushKernel：IPaintKernel 的插拔点①真实实现（B3-1）。经典 Pimpl：
// 对外仅 std::unique_ptr<Impl>，Impl 定义留在 brush_kernel.cpp。
//
// 经 unordered_map<BrushHandle, unique_ptr<Brush>> 拥有全部笔刷（RAII，析构自动释放）；
// createBrush 对全零 BrushParams 落 Brush 默认预设（风险 R3）；RNG 由 Brush 持有
// （Mt19937Random，seed 经构造/SetSeed 重播种）。
class BrushKernel : public IPaintKernel {
public:
    explicit BrushKernel(std::uint64_t seed = 0);
    ~BrushKernel() override;

    BrushKernel(const BrushKernel&) = delete;
    BrushKernel& operator=(const BrushKernel&) = delete;

    BrushHandle createBrush(const BrushParams& params) override;
    void beginStroke(BrushHandle brush, const StrokePoint& point) override;
    std::vector<StampData> strokeTo(BrushHandle brush, const StrokePoint& point) override;
    void endStroke(BrushHandle brush) override;

    // 重播种：更新后续 createBrush 的 seed，并对已建笔刷 reseed。
    // 约定：仅引擎空闲 / stroke 前调用（与 brush 线程 strokeTo 并发访问 RNG 有数据竞争，风险 R5）。
    void SetSeed(std::uint64_t seed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

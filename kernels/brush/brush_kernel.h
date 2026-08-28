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

    // 设置笔刷基础色（straight RGBA，0..1；a 为 ABI 兼容保留，本期内核只用 RGB）。
    // 内部 RGB→HSV 后转发到 Brush::setColor。句柄不存在时 no-op（不崩）。
    // 约定：仅引擎空闲 / stroke 前调用——颜色由消费端/UI 线程写入，brush 线程 strokeTo
    // 读同一 Brush 的 ColorH/S/V，跨线程变更（与 SetSeed 同款并发 caveat，风险 R5）。
    void setBrushColor(BrushHandle brush, float r, float g, float b, float a) override;

    // 设置笔刷基础参数（bugfix）：按句柄查 Brush 并调 Brush::setBase(id, value)，实时生效
    // 于下一笔 strokeTo。句柄不存在时 no-op（不崩）。约定：仅引擎空闲 / stroke 前调用——
    // 消费端/UI 线程写入 base_value，brush 线程 strokeTo 读同一值，与 setBrushColor 同款
    // 跨线程变更 caveat（风险 R5）。
    void setBrushSetting(BrushHandle brush, brush::SettingId id, float value) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

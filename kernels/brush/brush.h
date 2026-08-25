#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/types.h"
#include "kernels/brush/brush_settings.h"

class IRandomSource;

// Brush：自研 C++ 笔刷内核（B3-1，路线E §3a 主链）。经典 Pimpl：
// 对外仅持有 std::unique_ptr<Impl>，Impl 定义留在 brush.cpp，不暴露内部类型。
//
// 主链：beginStroke → strokeTo → (countDabsTo 求和 + 传感器滤波 + 设置映射 +
// prepareAndDrawDab 颜色调制) → endStroke。RNG 由 std::unique_ptr<IRandomSource> 持有
// （复用 core/determinism.h，生产 Mt19937Random、测试 ReplayRandom 可注入）。
//
// 非 Pimpl 暴露的唯一内部类型是 BrushParams/StrokePoint/StampData（core/types.h 内部共享，
// 不进 C ABI 头）。
class Brush {
public:
    // 对 radius<=0 / hardness<=0 的非法参数落默认预设（radius≈10px、hardness 0.7、
    // opacity 1.0、黑色），避免引擎 brushLoop 用全零 BrushParams{} 建出不可见 dab（风险 R3）。
    explicit Brush(std::unique_ptr<IRandomSource> rng, const BrushParams& params = BrushParams{});
    ~Brush();

    Brush(const Brush&) = delete;
    Brush& operator=(const Brush&) = delete;
    Brush(Brush&&) = delete;
    Brush& operator=(Brush&&) = delete;

    void beginStroke(const StrokePoint& p);
    std::vector<StampData> strokeTo(const StrokePoint& p);
    void endStroke();

    // 基础色（HSV）——供测试/后续 dgcSetBrushColor 接线。h ∈ [0,360)，s/v ∈ [0,1]。
    void setColor(float h, float s, float v);

    // 重播种 RNG（仅生产路径调用；ReplayRandom 注入时会被替换为 Mt19937Random）。
    void reseed(std::uint64_t seed);

    // 测试/调试：直接覆盖某设置的 base_value（含 dabs_per_second / offset_by_random 等）。
    void setBase(brush::SettingId id, float value);

    // 测试/调试：压力 → 半径响应曲线（x=归一化压力 0..1，y=radius_log 增量）。
    void setPressureRadiusCurve(const std::vector<brush::MappingPoint>& curve);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

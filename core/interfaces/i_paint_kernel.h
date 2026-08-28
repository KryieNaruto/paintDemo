#pragma once

#include <vector>

#include "core/types.h"
#include "kernels/brush/brush_settings.h"

// IPaintKernel：绘画内核抽象（笔迹输入 → StampData），纯虚接口，不对外。
class IPaintKernel {
public:
    virtual ~IPaintKernel() = default;
    virtual BrushHandle createBrush(const BrushParams&) = 0;
    virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;
    virtual std::vector<StampData> strokeTo(BrushHandle, const StrokePoint&) = 0;
    virtual void endStroke(BrushHandle) = 0;
    // 设置笔刷基础色（straight RGBA，0..1）。a 为 ABI 兼容保留，本期内核只用 RGB
    // （StampData 无 alpha 字段，不透明度走 setBrushSetting(DGC_SETTING_OPACITY)）。
    // 约定：仅引擎空闲 / stroke 前调用——颜色变更发生在消费端/UI 线程，而 brush 线程
    // strokeTo 读同一 Brush 的 ColorH/S/V，属跨线程变更（与 SetSeed 同款并发 caveat）。
    virtual void setBrushColor(BrushHandle, float r, float g, float b, float a) = 0;
    // 设置笔刷基础参数（bugfix：DGC_SETTING_RADIUS/HARDNESS/OPACITY/RADIUS_LOG 经此落地
    // 内核，走 Brush::setBase 直接改 base_value，下一笔 strokeTo 的 dab 即用新参数）。
    // 语义与 setBrushColor 同款：仅引擎空闲 / stroke 前调用；句柄不存在时实现 no-op。
    virtual void setBrushSetting(BrushHandle, brush::SettingId, float) = 0;
};

#pragma once

#include "core/interfaces/i_paint_kernel.h"

// NullPaintKernel：IPaintKernel 的空跑桩，strokeTo 返回空 vector。
class NullPaintKernel : public IPaintKernel {
public:
    BrushHandle createBrush(const BrushParams& params) override;
    void beginStroke(BrushHandle brush, const StrokePoint& point) override;
    std::vector<StampData> strokeTo(BrushHandle brush, const StrokePoint& point) override;
    void endStroke(BrushHandle brush) override;
    void setBrushColor(BrushHandle brush, float r, float g, float b, float a) override;
    // no-op：Null 桩无真实笔刷状态（bugfix：i_paint_kernel.h 新增纯虚后的空跑桩实现）。
    void setBrushSetting(BrushHandle brush, brush::SettingId id, float value) override;
};

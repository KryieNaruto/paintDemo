#pragma once

#include <vector>

#include "core/types.h"

// IPaintKernel：绘画内核抽象（笔迹输入 → StampData），纯虚接口，不对外。
class IPaintKernel {
public:
    virtual ~IPaintKernel() = default;
    virtual BrushHandle createBrush(const BrushParams&) = 0;
    virtual void beginStroke(BrushHandle, const StrokePoint&) = 0;
    virtual std::vector<StampData> strokeTo(BrushHandle, const StrokePoint&) = 0;
    virtual void endStroke(BrushHandle) = 0;
};

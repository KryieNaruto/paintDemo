#include "core/null/null_paint_kernel.h"

BrushHandle NullPaintKernel::createBrush(const BrushParams&) {
    return 0;
}

void NullPaintKernel::beginStroke(BrushHandle, const StrokePoint&) {}

std::vector<StampData> NullPaintKernel::strokeTo(BrushHandle, const StrokePoint&) {
    return {};
}

void NullPaintKernel::endStroke(BrushHandle) {}

void NullPaintKernel::setBrushColor(BrushHandle, float, float, float, float) {}

void NullPaintKernel::setBrushSetting(BrushHandle, brush::SettingId, float) {}

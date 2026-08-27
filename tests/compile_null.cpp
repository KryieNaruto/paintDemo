// host 编译 + 运行冒烟：实例化两个 Null 桩，断言 strokeTo 返回空、
// init/composite/present 等无副作用。
#include <cassert>
#include <vector>

#include "core/null/null_paint_kernel.h"
#include "core/null/null_render_backend.h"
#include "core/types.h"

int main() {
    NullPaintKernel kernel;
    NullRenderBackend backend;

    BrushHandle brush = kernel.createBrush(BrushParams{});
    kernel.beginStroke(brush, StrokePoint{});

    std::vector<StampData> stamps = kernel.strokeTo(brush, StrokePoint{});
    assert(stamps.empty());

    kernel.endStroke(brush);

    backend.init(nullptr, 0, 0);
    backend.resize(0, 0);
    backend.beginFrame();
    backend.composite({});
    backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);
    backend.present();
    backend.shutdown();

    return 0;
}

#include "kernels/brush/brush_kernel_factory.h"

#include <memory>

#include "core/null/null_paint_kernel.h"

#ifdef DGCPAIN_HAVE_BRUSH
#include "kernels/brush/brush_kernel.h"
#endif

std::unique_ptr<IPaintKernel> CreateDefaultPaintKernel() {
#ifdef DGCPAIN_HAVE_BRUSH
    return std::make_unique<BrushKernel>();
#else
    return std::make_unique<NullPaintKernel>();
#endif
}

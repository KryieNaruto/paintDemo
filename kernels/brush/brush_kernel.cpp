#include "kernels/brush/brush_kernel.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/determinism.h"
#include "kernels/brush/brush.h"

struct BrushKernel::Impl {
    std::unordered_map<BrushHandle, std::unique_ptr<Brush>> brushes;
    BrushHandle next_handle = 1;
    std::uint64_t seed = 0;
    std::mutex mutex;  // 守卫笔刷注册表与 seed（createBrush / SetSeed 互斥）。
};

BrushKernel::BrushKernel(std::uint64_t seed) : impl_(std::make_unique<Impl>()) {
    impl_->seed = seed;
}

BrushKernel::~BrushKernel() = default;

BrushHandle BrushKernel::createBrush(const BrushParams& params) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const BrushHandle handle = impl_->next_handle++;
    // Brush 对全零/非法参数落默认预设；RNG 由 Brush 经 unique_ptr<IRandomSource> 持有。
    impl_->brushes.emplace(handle, std::make_unique<Brush>(
                                       std::make_unique<Mt19937Random>(impl_->seed), params));
    return handle;
}

void BrushKernel::beginStroke(BrushHandle handle, const StrokePoint& point) {
    impl_->mutex.lock();
    auto it = impl_->brushes.find(handle);
    if (it == impl_->brushes.end()) {
        impl_->mutex.unlock();
        return;
    }
    Brush* brush = it->second.get();
    impl_->mutex.unlock();
    brush->beginStroke(point);
}

std::vector<StampData> BrushKernel::strokeTo(BrushHandle handle, const StrokePoint& point) {
    impl_->mutex.lock();
    auto it = impl_->brushes.find(handle);
    if (it == impl_->brushes.end()) {
        impl_->mutex.unlock();
        return {};
    }
    Brush* brush = it->second.get();
    impl_->mutex.unlock();
    return brush->strokeTo(point);
}

void BrushKernel::endStroke(BrushHandle handle) {
    impl_->mutex.lock();
    auto it = impl_->brushes.find(handle);
    if (it == impl_->brushes.end()) {
        impl_->mutex.unlock();
        return;
    }
    Brush* brush = it->second.get();
    impl_->mutex.unlock();
    brush->endStroke();
}

void BrushKernel::SetSeed(std::uint64_t seed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->seed = seed;
    for (auto& kv : impl_->brushes) {
        kv.second->reseed(seed);
    }
}

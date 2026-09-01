#pragma once

#include <cstdint>
#include <random>
#include <vector>

// §4.0.3 确定性状态：seed 注入内核 RNG；fixed_time_us 覆盖点流 t_us。
struct DeterminismState {
    std::uint64_t seed          = 0;
    double        fixed_time_us = 0.0;
    bool          override_time = false;
};

// 随机源与算法解耦（路线E §3a.5/§6.3）：生产用 Mt19937Random（可 seed 复现），
// 测试用 ReplayRandom（回放 oracle 序列）。二者分离，互不冲突。
class IRandomSource {
public:
    virtual ~IRandomSource() = default;
    virtual double nextUniform() = 0;   // [0,1)
    virtual double nextGauss()   = 0;   // 标准正态
};

class Mt19937Random final : public IRandomSource {
public:
    explicit Mt19937Random(std::uint64_t seed) : gen_(seed) {}
    void seed(std::uint64_t s) { gen_.seed(s); }
    double nextUniform() override { return uni_(gen_); }
    double nextGauss()   override { return gauss_(gen_); }
private:
    std::mt19937_64 gen_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::normal_distribution<double> gauss_{0.0, 1.0};
};

class ReplayRandom final : public IRandomSource {
public:
    explicit ReplayRandom(const std::vector<double>& seq) : seq_(seq) {}
    double nextUniform() override { return seq_[i_++]; }
    double nextGauss()   override { return seq_[i_++]; }
private:
    const std::vector<double>& seq_;
    std::size_t i_ = 0;
};

// 固定时间步进：override 时每个点 t_us = n * step_us（每笔画 beginStroke 归零），
// 消除真实时间戳的速度依赖抖动。
//
// bugfix（Fix A）：新增「默认步长」档位——非 override 且调用方显式给出 default_step_us>0
// 时（predictor 激活且未 dgcSetFixedTime），同样按 n * default_step_us 递增，避免给
// StrokeModeler 喂全 0 时间戳（把 PositionModeler 钉死在首点、笔画塌缩成点）。
//
// 取向记录（plan-review 建议 2）：§4.0.3「点流 t_us 时间戳」的「固定时间步进」取
// 「每笔画内 n*step 固定增量」语义（而非「每点同一绝对时间戳」），由 beginStroke
// 归零 n 作为笔画边界，故首点 t_us = 0、次点 = step、再点 = 2*step …
class FixedTimeStepper {
public:
    // 每笔画开始调用：记录步长、是否 override、默认步长，并把计数 n 归零。
    void beginStroke(double step_us, bool override_enabled, double default_step_us = 0.0) {
        step_         = step_us;
        override_     = override_enabled;
        default_step_ = default_step_us;
        n_            = 0;
    }
    // override 返回 n++ * step_；非 override 但有默认步长返回 n++ * default_step_；
    // 否则恒返回 0（无真实时间源，passthrough 默认路径）。
    std::uint64_t next() {
        if (override_) {
            return static_cast<std::uint64_t>(n_++ * step_);
        }
        if (default_step_ > 0.0) {
            return static_cast<std::uint64_t>(n_++ * default_step_);
        }
        return 0;
    }
private:
    double step_ = 0.0;
    double default_step_ = 0.0;
    bool override_ = false;
    std::uint64_t n_ = 0;
};

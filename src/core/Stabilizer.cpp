#include "core/Stabilizer.hpp"

#include <cmath>
#include <stdexcept>

namespace chroma::core {
namespace {
constexpr double kIdleResetMs = 250.0;  // spec §3
}

std::array<double, 2> windowMean(std::span<const TimedXY> v, double widthMs) {
    if (v.empty() || !(widthMs >= 0 && widthMs <= 200)) throw std::invalid_argument("window");
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto p = v[i];
        if (!(std::isfinite(p.ms) && p.x >= -65536 && p.x <= 98304 && p.y >= -65536 && p.y <= 98304 &&
              (i == 0 || p.ms > v[i - 1].ms))) {
            throw std::invalid_argument("sample");
        }
    }
    const auto end = v.back();
    const double begin = std::max(v.front().ms, end.ms - widthMs);
    if (end.ms <= begin) return {end.x, end.y};
    double sx = 0, sy = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const auto l = v[i - 1], r = v[i];
        if (r.ms <= begin) continue;
        const double left = std::max(l.ms, begin), f = (left - l.ms) / (r.ms - l.ms);
        const double half = (r.ms - left) * 0.5;
        sx += (std::lerp(l.x, r.x, f) + r.x) * half;
        sy += (std::lerp(l.y, r.y, f) + r.y) * half;
    }
    return {sx / (end.ms - begin), sy / (end.ms - begin)};
}

Stabilizer::Stabilizer(StabilizerSettings settings) : settings_(settings) {
    if (!(settings_.timeMs >= 0 && settings_.timeMs <= 200)) throw std::invalid_argument("stabilizer time");
}

V Stabilizer::push(double timeMs, V raw) {
    if (!std::isfinite(timeMs)) throw std::invalid_argument("filter time");
    if (!started_) {
        started_ = true;
        lastMs_ = timeMs;
        lastDtMs_ = 0.0;
        output_ = raw;
        beforeLast_ = raw;
        history_.assign(1, TimedXY{timeMs, raw.x, raw.y});
        return output_;
    }
    if (timeMs < lastMs_) return output_;  // late packet: already processed

    const bool sameTime = timeMs == lastMs_;
    switch (settings_.method) {
    case StabilizerMethod::Off:
        output_ = raw;
        break;
    case StabilizerMethod::Ema:
        if (sameTime) {
            output_ = ema(beforeLast_, raw, lastDtMs_, settings_.timeMs);
        } else {
            beforeLast_ = output_;
            lastDtMs_ = timeMs - lastMs_;
            output_ = ema(output_, raw, lastDtMs_, settings_.timeMs);
        }
        break;
    case StabilizerMethod::Window: {
        if (sameTime) {
            history_.back() = TimedXY{timeMs, raw.x, raw.y};
        } else {
            if (timeMs - lastMs_ > kIdleResetMs) history_.clear();
            history_.push_back(TimedXY{timeMs, raw.x, raw.y});
        }
        // Keep exactly one sample at or before the window start.
        const double start = timeMs - settings_.timeMs;
        std::size_t drop = 0;
        while (drop + 1 < history_.size() && history_[drop + 1].ms <= start) ++drop;
        if (drop > 0) history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop));
        const auto mean = windowMean(history_, settings_.timeMs);
        output_ = {mean[0], mean[1]};
        break;
    }
    }
    lastMs_ = timeMs;
    return output_;
}

}  // namespace chroma::core

#include "core/Fx.hpp"

#include <algorithm>
#include <cmath>

namespace chroma::core {
namespace {

double clampOr(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

}  // namespace

FxSettings defaultFx(FxKind kind) {
    FxSettings s;
    s.kind = kind;
    switch (kind) {
    case FxKind::Gaussian: s.radius = 6; break;
    case FxKind::Unsharp: s.radius = 3; break;
    case FxKind::AnimeLines: s.radius = 0; break;
    default: s.radius = 0; break;
    }
    return s;
}

std::string_view fxName(FxKind kind) noexcept {
    switch (kind) {
    case FxKind::Gaussian: return "Размытие по Гауссу";
    case FxKind::ToneCurve: return "Кривые";
    case FxKind::Unsharp: return "Нерезкая маска";
    case FxKind::Chromatic: return "Хроматическая аберрация";
    case FxKind::AnimeLines: return "Извлечение линий";
    }
    return {};
}

bool validCurve(const std::vector<CurvePoint>& points) {
    if (points.size() < 2 || points.size() > 32) return false;
    if (points.front().x != 0.0 || points.back().x != 1.0) return false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].y) || points[i].y < 0.0 || points[i].y > 1.0) return false;
        if (i > 0 && !(points[i].x > points[i - 1].x)) return false;
    }
    return true;
}

double evalCurve(const std::vector<CurvePoint>& points, double x) {
    if (points.empty()) return x;
    x = std::clamp(x, 0.0, 1.0);
    if (x <= points.front().x) return points.front().y;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const CurvePoint& a = points[i];
        const CurvePoint& b = points[i + 1];
        if (x <= b.x) return a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
    }
    return points.back().y;
}

FxSettings sanitize(const FxSettings& in) {
    FxSettings s = in;
    const FxSettings d = defaultFx(in.kind);
    const int maxRadius = in.kind == FxKind::AnimeLines ? 8 : 32;
    s.radius = std::clamp(in.radius, 0, maxRadius);
    s.gain = clampOr(in.gain, 0.0, 4.0, d.gain);
    s.threshold = clampOr(in.threshold, 0.0, 1.0, d.threshold);
    s.shiftX = clampOr(in.shiftX, -32.0, 32.0, d.shiftX);
    s.shiftY = clampOr(in.shiftY, -32.0, 32.0, d.shiftY);
    s.blackPoint = clampOr(in.blackPoint, 0.0, 1.0 - 1.0 / 255.0, d.blackPoint);
    s.whitePoint = clampOr(in.whitePoint, s.blackPoint + 1.0 / 255.0, 1.0, d.whitePoint);
    s.mid = clampOr(in.mid, 0.01, 0.99, d.mid);
    s.edgeGain = clampOr(in.edgeGain, 0.0, 4.0, d.edgeGain);
    s.amount = clampOr(in.amount, 0.0, 1.0, 1.0);
    for (auto& channel : s.curve.channels) {
        if (!validCurve(channel)) channel = {{0, 0}, {1, 1}};
    }
    return s;
}

int fxHalo(const FxSettings& s) {
    switch (s.kind) {
    case FxKind::Gaussian:
    case FxKind::Unsharp: return std::clamp(s.radius, 0, 32);
    case FxKind::ToneCurve: return 0;
    case FxKind::Chromatic: return static_cast<int>(std::ceil(std::max(std::abs(s.shiftX), std::abs(s.shiftY)))) + 1;
    case FxKind::AnimeLines: return std::clamp(s.radius, 0, 8) + 1;
    }
    return 0;
}

bool fxSpreads(const FxSettings& s) { return s.kind == FxKind::Gaussian || s.kind == FxKind::AnimeLines; }

std::array<float, 256 * 4> curveTable(const ToneCurve& curve) {
    std::array<float, 256 * 4> table{};
    for (int i = 0; i < 256; ++i) {
        const double m = evalCurve(curve.channels[0], i / 255.0);
        for (int c = 0; c < 3; ++c) {
            table[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(c)] =
                static_cast<float>(evalCurve(curve.channels[static_cast<std::size_t>(c) + 1], m));
        }
        table[static_cast<std::size_t>(i) * 4 + 3] = 1.0f;
    }
    return table;
}

}  // namespace chroma::core

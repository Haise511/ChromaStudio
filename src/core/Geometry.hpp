#pragma once
// ChromaStudio Engine · core · stroke geometry (spec §3.1).
//
// The §3.1 listing is normative: uniform Catmull-Rom in Bézier form with one-sided tangents at the
// ends, arc-length stepping in fractions of the diameter with a positive carry, linearisation
// tolerance 0.125 document px. Two additions make it usable while the pen is still moving:
//   * Node also carries tilt, interpolated with the same parameter as pressure (§3: geometry and
//     attributes are interpolated separately; tilt axes linearly).
//   * StrokeWalker runs the same loop incrementally. Segment i is final once node i+2 exists (its
//     end tangent is then known); the last segment is provisional and can be previewed without
//     touching the step state. walk() is StrokeWalker run to completion, so both give identical marks.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace chroma::core {

struct V {
    double x = 0.0;
    double y = 0.0;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
inline V operator*(V a, double k) { return {a.x * k, a.y * k}; }
inline bool operator==(V a, V b) { return a.x == b.x && a.y == b.y; }
inline double norm(V v) { return std::hypot(v.x, v.y); }
inline double unit(double x) { return std::clamp(x, 0.0, 1.0); }
inline double smooth(double x) {
    x = unit(x);
    return x * x * (3.0 - 2.0 * x);
}

inline constexpr double kWorkspaceMin = -65536.0;  // §3: numerical workspace per axis
inline constexpr double kWorkspaceMax = 98304.0;
inline constexpr double kFlatnessPx = 0.125;  // §3.1: linear approximation tolerance

struct Node {
    V xy;
    double pressure = 1.0;
    double tiltX = 0.0;  // radians
    double tiltY = 0.0;
};

struct Mark {
    Node node;
    double arc = 0.0;
};

using PressurePoint = std::array<double, 2>;

struct Style {
    double diameter = 12.0;
    double minSize = 0.0;
    double spacing = 0.1;
    std::span<const PressurePoint> sizeCurve{};
    std::span<const PressurePoint> opacityCurve{};
};

inline void validateCurve(std::span<const PressurePoint> c) {
    if (c.empty()) return;
    if (c.size() < 2 || c.size() > 32 || c.front()[0] != 0 || c.back()[0] != 1) {
        throw std::invalid_argument("pressure endpoints");
    }
    double previous = -1;
    for (auto [x, y] : c) {
        if (!(x > previous && x <= 1 && x >= 0 && y >= 0 && y <= 1)) throw std::invalid_argument("pressure curve");
        previous = x;
    }
}

inline double pressureGain(double p, std::span<const PressurePoint> curve) {
    p = unit(p);
    if (curve.empty()) return smooth(p);
    auto r = std::lower_bound(curve.begin(), curve.end(), p,
                              [](const PressurePoint& v, double x) { return v[0] < x; });
    if (r == curve.begin()) return r->at(1);
    if (r == curve.end()) return curve.back()[1];
    auto l = r - 1;
    return std::lerp((*l)[1], (*r)[1], (p - (*l)[0]) / ((*r)[0] - (*l)[0]));
}

inline double opacityAt(const Style& s, double p, double base, double minimum) {
    return unit(base) * std::lerp(unit(minimum), 1.0, pressureGain(p, s.opacityCurve));
}

inline V ema(V previous, V input, double dtMs, double tauMs) {
    if (!(dtMs >= 0 && tauMs >= 0 && std::isfinite(dtMs) && std::isfinite(tauMs))) {
        throw std::invalid_argument("filter time");
    }
    if (tauMs == 0 || dtMs > 250) return input;
    return previous + (input - previous) * (-std::expm1(-dtMs / tauMs));
}

inline V bezier(V a, V b, V c, V d, double t) {
    const double u = 1 - t;
    return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
}

inline double diameter(const Style& s, double p) {
    return s.diameter * (s.minSize + (1 - s.minSize) * pressureGain(p, s.sizeCurve));
}

inline double envelope(double s, double length, double start, double end, double velocity, double gain, bool linear) {
    if (length <= 0) return 1;
    end *= 1 + gain * unit((velocity - 200) / 1000);
    const double total = start + end;
    if (total > length) {
        start *= length / total;
        end *= length / total;
    }
    const double f = std::min(start > 0 ? unit(s / start) : 1.0, end > 0 ? unit((length - s) / end) : 1.0);
    return linear ? f : smooth(f);
}

inline void validateStyle(const Style& style) {
    if (!(style.diameter >= 0.25 && style.diameter <= 2048 && style.minSize >= 0 && style.minSize <= 1 &&
          style.spacing >= 0.01 && style.spacing <= 2)) {
        throw std::invalid_argument("brush geometry");
    }
    validateCurve(style.sizeCurve);
    validateCurve(style.opacityCurve);
}

inline void validateNode(const Node& v) {
    if (!(v.xy.x >= kWorkspaceMin && v.xy.x <= kWorkspaceMax && v.xy.y >= kWorkspaceMin && v.xy.y <= kWorkspaceMax &&
          v.pressure >= 0 && v.pressure <= 1 && std::isfinite(v.tiltX) && std::isfinite(v.tiltY))) {
        throw std::invalid_argument("input node");
    }
}

/// The §3.1 walk, fed one stabilised node at a time.
class StrokeWalker {
public:
    explicit StrokeWalker(const Style& style) : style_(style) { validateStyle(style_); }

    /// Appends a node and emits the marks of every segment that became final. The caller merges
    /// equal neighbouring positions first (updateLast).
    template <class Emit>
    void append(const Node& node, Emit&& emit) {
        validateNode(node);
        nodes_.push_back(node);
        if (nodes_.size() == 1) {
            state_.remaining = step(node.pressure);
            emit(Mark{node, 0.0});
            return;
        }
        while (next_ + 2 < nodes_.size()) segment(next_++, state_, emit);
    }

    /// Replaces pressure and tilt of the last node (same position). Its segments are not final yet.
    void updateLast(const Node& node) {
        validateNode(node);
        if (nodes_.empty()) return;
        nodes_.back().pressure = node.pressure;
        nodes_.back().tiltX = node.tiltX;
        nodes_.back().tiltY = node.tiltY;
    }

    /// Marks of the segments that are not final yet, optionally extended by one pending node.
    /// The step state is not advanced.
    template <class Emit>
    void previewTail(const Node* pending, Emit&& emit) {
        if (pending) {
            validateNode(*pending);
            nodes_.push_back(*pending);
        }
        struct PopGuard {
            std::vector<Node>* nodes;
            bool active;
            ~PopGuard() {
                if (active) nodes->pop_back();
            }
        } guard{&nodes_, pending != nullptr};
        State state = state_;
        for (std::size_t i = next_; i + 1 < nodes_.size(); ++i) segment(i, state, emit);
    }

    /// Emits the remaining segments and the end mark; returns the stroke length L.
    template <class Emit>
    double finish(Emit&& emit) {
        if (nodes_.empty()) return 0.0;
        for (; next_ + 1 < nodes_.size(); ++next_) segment(next_, state_, emit);
        if (state_.arc - state_.lastMark > 1e-8) emit(Mark{nodes_.back(), state_.arc});
        return state_.arc;
    }

    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }
    [[nodiscard]] const Style& style() const noexcept { return style_; }

private:
    struct State {
        double remaining = 0.0;
        double arc = 0.0;
        double lastMark = 0.0;
    };

    [[nodiscard]] double step(double p) const { return std::max(0.25, diameter(style_, p) * style_.spacing); }

    // One iteration of the §3.1 segment loop.
    template <class Emit>
    void segment(std::size_t i, State& st, Emit& emit) const {
        const Node left = nodes_[i];
        const Node right = nodes_[i + 1];
        const V a = left.xy;
        const V d = right.xy;
        if (norm(d - a) == 0) return;
        const V m0 = (i == 0) ? d - a : (d - nodes_[i - 1].xy) * 0.5;
        const V m1 = (i + 2 == nodes_.size()) ? d - a : (nodes_[i + 2].xy - a) * 0.5;
        const V b = a + m0 * (1.0 / 3);
        const V c = d - m1 * (1.0 / 3);
        const double bound = 6 * std::max(norm(c - b * 2 + a), norm(d - c * 2 + b));
        const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(bound / (8 * kFlatnessPx)))));
        V prev = a;
        for (int j = 1; j <= n; ++j) {
            const V next = bezier(a, b, c, d, static_cast<double>(j) / n);
            const V delta = next - prev;
            const double len = norm(delta);
            double advanced = 0;
            while (len - advanced >= st.remaining) {
                advanced += st.remaining;
                const double f = advanced / len;
                const double t = (j - 1 + f) / n;
                const Node sample{prev + delta * f, std::lerp(left.pressure, right.pressure, t),
                                  std::lerp(left.tiltX, right.tiltX, t), std::lerp(left.tiltY, right.tiltY, t)};
                st.lastMark = st.arc + advanced;
                emit(Mark{sample, st.lastMark});
                st.remaining = step(sample.pressure);
            }
            st.remaining -= len - advanced;
            st.arc += len;
            prev = next;
        }
    }

    Style style_;
    std::vector<Node> nodes_;
    State state_{};
    std::size_t next_ = 0;  // first segment that is not final yet
};

/// §3.1 walk: a pure, repeatable pass. Call once with an empty consumer for L, then with the stamp
/// consumer.
template <class Emit>
double walk(std::span<const Node> points, const Style& style, Emit emit) {
    validateStyle(style);
    for (const Node& v : points) validateNode(v);
    StrokeWalker walker(style);
    for (const Node& v : points) walker.append(v, emit);
    return walker.finish(emit);
}

}  // namespace chroma::core

#include "core/StrokeBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace chroma::core {
namespace {

constexpr double kVelocityWindowMs = 30.0;  // spec §3
constexpr double kIdleLiftMs = 250.0;
constexpr double kMaxVelocity = 4000.0;
constexpr double kTiltLimit = std::numbers::pi / 2 - 0.001;  // spec §4

Style makeStyle(const BrushProfile& p, double diameterPx) {
    Style s;
    s.diameter = std::clamp(diameterPx, 0.25, 2048.0);
    s.minSize = p.ink.minSize;
    s.spacing = p.ink.spacing;
    s.sizeCurve = p.sizeCurve;
    s.opacityCurve = p.opacityCurve;
    return s;
}

V clampToWorkspace(V v) {
    return {std::clamp(v.x, kWorkspaceMin, kWorkspaceMax), std::clamp(v.y, kWorkspaceMin, kWorkspaceMax)};
}

}  // namespace

// profile_ is declared before style_ and walker_, so the curve spans point into this object.
StrokeBuilder::StrokeBuilder(const BrushProfile& profile, double diameterPx)
    : profile_(profile),
      style_(makeStyle(profile_, diameterPx)),
      stabilizer_(profile_.stabilizer),
      walker_(style_) {
    validate(profile_);
}

double StrokeBuilder::previewEnvelope(double arc) const {
    const double start = profile_.taper.startPx;
    if (start <= 0) return 1.0;
    const double f = unit(arc / start);
    return profile_.taper.shape == TaperShape::Linear ? f : smooth(f);
}

Dab StrokeBuilder::makeDab(const Mark& mark, double f, Orientation& orientation, bool touch) const {
    const Node& node = mark.node;
    Dab dab;
    dab.center = node.xy;
    dab.arc = mark.arc;
    dab.aspect = profile_.tip.aspect;
    // §3: separate powers of f; a zero power gives 1 even at f = 0 (std::pow(0, 0) == 1).
    dab.radius = 0.5 * diameter(style_, node.pressure) * std::pow(f, profile_.taper.radiusPower);
    dab.inkWeight = opacityAt(style_, node.pressure, profile_.ink.opacity, profile_.ink.minOpacity) *
                    std::pow(f, profile_.taper.opacityPower) * profile_.ink.flow;

    // §4: angleDeg plus the argument of (tan tiltX, tan tiltY) or of the tangent; a zero vector keeps
    // the last non-zero angle, a single touch uses the given angle only.
    double angle = profile_.tip.angleDeg * std::numbers::pi / 180.0;
    if (!touch) {
        if (profile_.tip.orientation == TipOrientation::Tilt) {
            const double tx = std::tan(std::clamp(node.tiltX, -kTiltLimit, kTiltLimit));
            const double ty = std::tan(std::clamp(node.tiltY, -kTiltLimit, kTiltLimit));
            if (tx != 0.0 || ty != 0.0) orientation.tilt = std::atan2(ty, tx);
            angle += orientation.tilt;
        } else if (profile_.tip.orientation == TipOrientation::Path) {
            if (orientation.hasPosition) {
                const V d = node.xy - orientation.lastPosition;
                if (d.x != 0.0 || d.y != 0.0) orientation.path = std::atan2(d.y, d.x);
            }
            angle += orientation.path;
        }
    }
    orientation.lastPosition = node.xy;
    orientation.hasPosition = true;
    dab.angle = angle;
    return dab;
}

void StrokeBuilder::appendNode(const Node& node, std::vector<Dab>& out) {
    walker_.append(node, [&](const Mark& m) { out.push_back(makeDab(m, previewEnvelope(m.arc), orientation_, false)); });
}

void StrokeBuilder::commitPending(std::vector<Dab>& out) {
    if (!pending_) return;
    const Node node = *pending_;
    pending_.reset();
    const auto nodes = walker_.nodes();
    if (!nodes.empty() && nodes.back().xy == node.xy) {
        walker_.updateLast(node);  // §3.1: equal neighbours merge, keeping the latest contact pressure
        return;
    }
    appendNode(node, out);
}

void StrokeBuilder::add(const StrokeSample& in, std::vector<Dab>& finalDabs) {
    if (!(std::isfinite(in.timeMs) && std::isfinite(in.position.x) && std::isfinite(in.position.y) &&
          std::isfinite(in.pressure) && std::isfinite(in.tiltX) && std::isfinite(in.tiltY))) {
        return;  // §3: only finite values enter the pipeline
    }
    if (!raw_.empty() && in.timeMs < raw_.back().timeMs) return;  // late packet

    StrokeSample sample = in;
    sample.position = clampToWorkspace(in.position);
    sample.pressure = unit(in.pressure);
    if (!raw_.empty() && sample.timeMs == raw_.back().timeMs) {
        raw_.back() = sample;
    } else {
        raw_.push_back(sample);
    }

    const V filtered = stabilizer_.push(sample.timeMs, sample.position);
    const Node node{filtered, sample.pressure, sample.tiltX, sample.tiltY};
    if (pending_ && sample.timeMs == pendingMs_) {
        pending_ = node;  // same time: the last position wins
        return;
    }
    commitPending(finalDabs);
    pending_ = node;
    pendingMs_ = sample.timeMs;
}

void StrokeBuilder::tail(std::vector<Dab>& tailDabs) {
    tailDabs.clear();
    Orientation orientation = orientation_;
    const auto sink = [&](const Mark& m) {
        tailDabs.push_back(makeDab(m, previewEnvelope(m.arc), orientation, false));
    };
    if (walker_.nodes().empty()) {
        if (pending_) sink(Mark{*pending_, 0.0});
        return;
    }
    walker_.previewTail(pending_ ? &*pending_ : nullptr, sink);
}

double StrokeBuilder::liftVelocity() const {
    if (raw_.size() < 2) return 0.0;
    const double end = raw_.back().timeMs;
    std::size_t lastMove = 0;
    for (std::size_t i = raw_.size() - 1; i > 0; --i) {
        if (!(raw_[i].position == raw_[i - 1].position)) {
            lastMove = i;
            break;
        }
    }
    if (lastMove == 0) return 0.0;                                   // no movement at all
    if (end - raw_[lastMove].timeMs > kIdleLiftMs) return 0.0;       // paused before lifting

    // Length of the raw contact polyline in the last 30 ms over its duration (≥ 1 ms).
    const double windowStart = end - kVelocityWindowMs;
    double length = 0.0;
    double covered = end;
    for (std::size_t i = raw_.size() - 1; i > 0; --i) {
        const StrokeSample& a = raw_[i - 1];
        const StrokeSample& b = raw_[i];
        if (b.timeMs <= windowStart) break;
        const double from = std::max(a.timeMs, windowStart);
        const double span = b.timeMs - a.timeMs;
        const double fraction = span > 0 ? (b.timeMs - from) / span : 1.0;
        length += norm(b.position - a.position) * fraction;
        covered = from;
    }
    const double duration = std::max(1.0, end - covered);
    return std::min(length / duration * 1000.0, kMaxVelocity);
}

StrokeResult StrokeBuilder::finish(const StrokeSample& lift) {
    StrokeResult result;
    std::vector<Dab>& appended = result.dabs;
    add(lift, appended);
    commitPending(appended);

    // §3: on up, write the real last position when the filtered end differs from it.
    if (!raw_.empty() && !walker_.nodes().empty()) {
        const StrokeSample& last = raw_.back();
        const Node end{last.position, last.pressure, last.tiltX, last.tiltY};
        if (!(walker_.nodes().back().xy == end.xy)) appendNode(end, appended);
    }

    result.liftVelocity = liftVelocity();
    const double length = walker_.finish(
        [&](const Mark& m) { appended.push_back(makeDab(m, previewEnvelope(m.arc), orientation_, false)); });
    result.length = length;

    const Taper& taper = profile_.taper;
    const bool envelopeIgnored = taper.radiusPower == 0 && taper.opacityPower == 0;
    const bool previewIsFinal = taper.endPx == 0 && taper.startPx <= length;
    if (length > 0 && (envelopeIgnored || previewIsFinal)) return result;  // incremental dabs are final

    // Replay: the complete stroke with the final envelope.
    std::vector<Mark> marks;
    walk(walker_.nodes(), style_, [&](const Mark& m) { marks.push_back(m); });
    const bool linear = taper.shape == TaperShape::Linear;
    const bool touch = length <= 0.0;
    result.dabs.clear();
    result.replay = true;

    Orientation orientation;
    bool allZero = true;
    for (const Mark& m : marks) {
        const double f =
            envelope(m.arc, length, taper.startPx, taper.endPx, result.liftVelocity, taper.liftGain, linear);
        if (f > 0) allZero = false;
        result.dabs.push_back(makeDab(m, f, orientation, touch));
    }

    // §3: a short path with both tapers whose regular steps left only zero ends gets one stamp at the
    // apex of the envelope: s = I·L/(I+E) when the arms cross, else the middle of the plateau.
    if (allZero && !envelopeIgnored && taper.startPx > 0 && taper.endPx > 0 && marks.size() >= 2) {
        const double end = taper.endPx * (1 + taper.liftGain * unit((result.liftVelocity - 200) / 1000));
        const double total = taper.startPx + end;
        const double apex = total > length ? taper.startPx * length / total : 0.5 * (taper.startPx + length - end);
        const auto right = std::lower_bound(marks.begin(), marks.end(), apex,
                                            [](const Mark& m, double s) { return m.arc < s; });
        if (right != marks.begin() && right != marks.end()) {
            const Mark& a = *(right - 1);
            const Mark& b = *right;
            const double t = b.arc > a.arc ? (apex - a.arc) / (b.arc - a.arc) : 0.0;
            const Node node{a.node.xy + (b.node.xy - a.node.xy) * t, std::lerp(a.node.pressure, b.node.pressure, t),
                            std::lerp(a.node.tiltX, b.node.tiltX, t), std::lerp(a.node.tiltY, b.node.tiltY, t)};
            const double f =
                envelope(apex, length, taper.startPx, taper.endPx, result.liftVelocity, taper.liftGain, linear);
            Orientation apexOrientation = orientation;
            result.dabs.push_back(makeDab(Mark{node, apex}, f, apexOrientation, false));
        }
    }
    return result;
}

}  // namespace chroma::core

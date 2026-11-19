#include "core/Vector.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace chroma::core {
namespace {

constexpr double kPi = std::numbers::pi;

double shortestArc(double a, double b, double t) {
    double d = std::remainder(b - a, 2 * kPi);
    if (d == -kPi) d = kPi;
    return std::remainder(a + d * t, 2 * kPi);
}

/// Number of linear pieces for a segment within the §3.1 flatness.
int pieces(V a, V b, V c, V d) {
    const double bound = 6 * std::max(norm(c - b * 2 + a), norm(d - c * 2 + b));
    return std::max(1, static_cast<int>(std::ceil(std::sqrt(bound / (8 * kFlatnessPx)))));
}

double distanceToChord(V p, V a, V b) {
    const V ab = b - a;
    const double len = norm(ab);
    if (len == 0) return norm(p - a);
    const double t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / (len * len), 0.0, 1.0);
    return norm(p - (a + ab * t));
}

void subdivide(std::span<const Dab> dabs, std::size_t i0, std::size_t i1, int depth, std::vector<std::size_t>& keep) {
    if (i1 <= i0 + 1 || depth >= 16) return;
    const double a0 = dabs[i0].arc, a1 = dabs[i1].arc;
    bool split = false;
    for (std::size_t k = i0 + 1; k < i1 && !split; ++k) {
        const double t = a1 > a0 ? (dabs[k].arc - a0) / (a1 - a0) : 0.5;
        const double w = std::lerp(2 * dabs[i0].radius, 2 * dabs[i1].radius, t);
        const double o = std::lerp(dabs[i0].inkWeight, dabs[i1].inkWeight, t);
        split = std::abs(2 * dabs[k].radius - w) > 0.125 || std::abs(dabs[k].inkWeight - o) > 1.0 / 255.0 ||
                distanceToChord(dabs[k].center, dabs[i0].center, dabs[i1].center) > 0.5;
    }
    if (!split) return;
    const std::size_t mid = (i0 + i1) / 2;
    subdivide(dabs, i0, mid, depth + 1, keep);
    keep.push_back(mid);
    subdivide(dabs, mid, i1, depth + 1, keep);
}

Knot knotFrom(const Dab& d, std::uint64_t id) {
    const double width = std::clamp(2 * d.radius, 0.0, 2048.0);
    return Knot{id, d.center, {0, 0}, {0, 0}, width, std::clamp(width, 0.25, 2048.0), std::remainder(d.angle, 2 * kPi),
                std::clamp(d.inkWeight, 0.0, 1.0), Join::smooth};
}

}  // namespace

std::vector<Knot> fitKnots(std::span<const Dab> dabs, const std::function<std::uint64_t()>& nextId) {
    std::vector<Knot> knots;
    if (dabs.empty()) return knots;
    if (dabs.size() == 1) {
        knots.push_back(knotFrom(dabs[0], nextId()));
        return knots;
    }
    std::vector<std::size_t> keep{0};
    subdivide(dabs, 0, dabs.size() - 1, 0, keep);
    keep.push_back(dabs.size() - 1);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    for (const std::size_t i : keep) knots.push_back(knotFrom(dabs[i], nextId()));
    // Handles: chord-length tangents along the neighbours.
    for (std::size_t j = 0; j < knots.size(); ++j) {
        const V p = knots[j].pos;
        const V prev = j > 0 ? knots[j - 1].pos : p;
        const V next = j + 1 < knots.size() ? knots[j + 1].pos : p;
        const V dir = next - prev;
        const double len = norm(dir);
        if (len == 0) continue;
        const V u = dir * (1.0 / len);
        knots[j].out = j + 1 < knots.size() ? u * (norm(next - p) / 3.0) : V{0, 0};
        knots[j].in = j > 0 ? u * (-norm(p - prev) / 3.0) : V{0, 0};
    }
    return knots;
}

std::vector<CurveSample> flatten(const VectorObject& o) {
    std::vector<CurveSample> out;
    if (o.knots.empty()) return out;
    out.push_back({o.knots[0].pos, 0, 0.0, o.knots[0].width});
    for (std::size_t i = 0; i + 1 < o.knots.size(); ++i) {
        const Knot& l = o.knots[i];
        const Knot& r = o.knots[i + 1];
        const V a = l.pos, b = l.pos + l.out, c = r.pos + r.in, d = r.pos;
        const int n = pieces(a, b, c, d);
        for (int j = 1; j <= n; ++j) {
            const double t = static_cast<double>(j) / n;
            out.push_back({bezier(a, b, c, d, t), i, t, std::lerp(l.width, r.width, t)});
        }
    }
    return out;
}

std::vector<Dab> objectDabs(const VectorObject& o) {
    std::vector<Dab> dabs;
    if (o.knots.empty()) return dabs;
    const double spacing = std::clamp(o.profile.ink.spacing, 0.01, 2.0);
    const double aspect = std::clamp(o.profile.tip.aspect, 0.05, 1.0);
    const auto stamp = [&](V at, double width, double opacity, double angle, double arc) {
        dabs.push_back(Dab{at, width / 2, aspect, angle, std::clamp(opacity, 0.0, 1.0), arc});
    };
    const Knot& first = o.knots.front();
    stamp(first.pos, first.width, first.opacity, first.angle, 0.0);  // a round start
    if (o.knots.size() == 1) return dabs;
    double arc = 0.0;
    double lastMark = 0.0;
    double remaining = std::max(0.25, first.spacingDiameter * spacing);
    for (std::size_t i = 0; i + 1 < o.knots.size(); ++i) {
        const Knot& l = o.knots[i];
        const Knot& r = o.knots[i + 1];
        const V a = l.pos, b = l.pos + l.out, c = r.pos + r.in, d = r.pos;
        const int n = pieces(a, b, c, d);
        V prev = a;
        for (int j = 1; j <= n; ++j) {
            const V next = bezier(a, b, c, d, static_cast<double>(j) / n);
            const V delta = next - prev;
            const double len = norm(delta);
            double advanced = 0;
            while (len > 0 && len - advanced >= remaining) {
                advanced += remaining;
                const double t = (j - 1 + advanced / len) / n;
                lastMark = arc + advanced;
                stamp(prev + delta * (advanced / len), std::lerp(l.width, r.width, t), std::lerp(l.opacity, r.opacity, t),
                      shortestArc(l.angle, r.angle, t), lastMark);
                remaining = std::max(0.25, std::lerp(l.spacingDiameter, r.spacingDiameter, t) * spacing);
            }
            remaining -= len - advanced;
            arc += len;
            prev = next;
        }
    }
    const Knot& last = o.knots.back();
    if (arc - lastMark > 1e-8) stamp(last.pos, last.width, last.opacity, last.angle, arc);  // a round end
    return dabs;
}

std::optional<CurveSample> nearest(const VectorObject& o, V p, double* distance) {
    const std::vector<CurveSample> samples = flatten(o);
    if (samples.empty()) return std::nullopt;
    std::optional<CurveSample> best;
    double bestD = 0.0;
    const auto consider = [&](const CurveSample& s, double d) {
        if (!best || d < bestD) {
            best = s;
            bestD = d;
        }
    };
    consider(samples[0], norm(p - samples[0].point));
    for (std::size_t k = 1; k < samples.size(); ++k) {
        const CurveSample& a = samples[k - 1];
        const CurveSample& b = samples[k];
        const V ab = b.point - a.point;
        const double len2 = ab.x * ab.x + ab.y * ab.y;
        // The projection on a zero piece is its start (§8).
        const double f = len2 > 0 ? std::clamp(((p.x - a.point.x) * ab.x + (p.y - a.point.y) * ab.y) / len2, 0.0, 1.0) : 0.0;
        const double t0 = b.segment == a.segment ? a.t : 0.0;
        CurveSample s{a.point + ab * f, b.segment, std::lerp(t0, b.t, f), std::lerp(a.width, b.width, f)};
        consider(s, norm(p - s.point));
    }
    if (distance) *distance = bestD;
    return best;
}

void setHandle(Knot& k, bool out, V h) {
    V& mine = out ? k.out : k.in;
    V& other = out ? k.in : k.out;
    mine = h;
    switch (k.join) {
    case Join::symmetric: other = h * -1.0; break;
    case Join::smooth: {
        const double len = norm(h);
        if (len > 0) other = h * (-norm(other) / len);  // zero length: keep the other handle as it is
        break;
    }
    case Join::corner: break;
    }
}

VectorObject insertKnot(const VectorObject& o, std::size_t segment, double t, std::uint64_t newId) {
    if (segment + 1 >= o.knots.size()) return o;
    VectorObject result = o;
    const auto parts = splitSegment(o.knots[segment], o.knots[segment + 1], t, newId);
    result.knots[segment] = parts[0];
    result.knots[segment + 1] = parts[2];
    result.knots.insert(result.knots.begin() + static_cast<std::ptrdiff_t>(segment + 1), parts[1]);
    ++result.version;
    return result;
}

std::optional<VectorObject> removeKnot(const VectorObject& o, std::size_t index) {
    if (index >= o.knots.size()) return o;
    if (o.knots.size() == 1) return std::nullopt;  // the last point removes the object
    VectorObject result = o;
    result.knots.erase(result.knots.begin() + static_cast<std::ptrdiff_t>(index));
    result.knots.front().in = {0, 0};
    result.knots.back().out = {0, 0};
    ++result.version;
    return result;
}

std::string validateObject(const VectorObject& o) {
    if (o.id == 0) return "id объекта не может быть нулём";
    if (o.version < 1) return "version должен быть ≥ 1";
    for (const double c : o.rgba) {
        if (!std::isfinite(c) || c < 0 || c > 1) return "rgba вне [0, 1]";
    }
    if (o.knots.empty() || o.knots.size() > 1000000) return "у объекта от 1 до 1 000 000 узлов";
    std::set<std::uint64_t> ids;
    for (const Knot& k : o.knots) {
        if (!valid(k)) return std::format("узел {} вне контракта §8.1", k.id);
        if (!ids.insert(k.id).second) return std::format("повтор id узла {}", k.id);
    }
    if (o.profile.family == BrushFamily::Watercolor) return "векторный объект хранит только сухой профиль";
    return {};
}

}  // namespace chroma::core

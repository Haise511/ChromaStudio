#pragma once
// ChromaStudio Engine · core · editable vector lineart (spec §8, §8.1).
//
// A vector layer holds ordered objects; an object keeps an immutable ID, a version, straight linear
// RGBA, a snapshot of the dry profile, a seed and its knots. A knot has its ID, position P, two
// handles in/out relative to P, the width after pressure and taper, the spacing diameter, the pen
// angle, the opacity and the join type. Segment i is the Bézier (Pᵢ, Pᵢ + outᵢ, Pᵢ₊₁ + inᵢ₊₁, Pᵢ₊₁);
// width, spacing diameter and opacity interpolate linearly in t, the angle along the shortest arc.
// For the screen and export an object is replayed from its geometry: the curve is flattened with
// the §3.1 tolerance and stamped in arc order with the same spacing rule, with round end stamps.

#include "core/BrushProfile.hpp"
#include "core/Geometry.hpp"
#include "core/StrokeBuilder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace chroma::core {

// ── §8.1, as listed ──────────────────────────────────────────────────────────

enum class Join { corner, smooth, symmetric };

struct Knot {
    std::uint64_t id;
    V pos, in, out;
    double width, spacingDiameter, angle, opacity;
    Join join;
    friend bool operator==(const Knot&, const Knot&) = default;
};

inline bool bounded(V p) { return p.x >= -65536 && p.x <= 98304 && p.y >= -65536 && p.y <= 98304; }

inline bool valid(const Knot& k) {
    return k.id != 0 && bounded(k.pos) && bounded(k.pos + k.in) && bounded(k.pos + k.out) && k.width >= 0 &&
           k.width <= 2048 && k.spacingDiameter >= .25 && k.spacingDiameter <= 2048 && std::isfinite(k.angle) &&
           std::abs(k.angle) <= std::numbers::pi && k.opacity >= 0 && k.opacity <= 1 &&
           (k.join == Join::corner || k.join == Join::smooth || k.join == Join::symmetric);
}

inline std::array<Knot, 3> splitSegment(Knot l, Knot r, double t, std::uint64_t newId) {
    if (!valid(l) || !valid(r) || !(t > 0 && t < 1) || l.id == r.id || newId == 0 || newId == l.id || newId == r.id)
        throw std::invalid_argument("vector split");
    auto mix = [t](V p, V q) { return p + (q - p) * t; };
    V a = mix(l.pos, l.pos + l.out), b = mix(l.pos + l.out, r.pos + r.in);
    V c = mix(r.pos + r.in, r.pos), d = mix(a, b), e = mix(b, c), f = mix(d, e);
    constexpr double pi = std::numbers::pi;
    double da = std::remainder(r.angle - l.angle, 2 * pi);
    if (da == -pi) da = pi;
    Knot mid{newId, f, d - f, e - f, std::lerp(l.width, r.width, t), std::lerp(l.spacingDiameter, r.spacingDiameter, t),
             std::remainder(l.angle + da * t, 2 * pi), std::lerp(l.opacity, r.opacity, t), Join::smooth};
    l.out = a - l.pos;
    r.in = c - r.pos;
    if (l.join == Join::symmetric) l.join = Join::smooth;
    if (r.join == Join::symmetric) r.join = Join::smooth;
    return {l, mid, r};
}

// ── Objects ──────────────────────────────────────────────────────────────────

struct VectorObject {
    std::uint64_t id = 0;
    std::uint64_t version = 1;
    std::array<double, 4> rgba{0, 0, 0, 1};  // straight, linear
    BrushProfile profile;                    // a dry profile
    std::uint32_t seed = 0;
    std::vector<Knot> knots;
    friend bool operator==(const VectorObject&, const VectorObject&) = default;
};

/// Objects of one vector layer, bottom to top.
using VectorLayer = std::vector<VectorObject>;

/// Knots for a drawn stroke from its final dabs: kept by recursive halving until width, opacity and
/// position between kept knots are linear within 0.125 px, 1/255 and 0.5 px (depth ≤ 16); handles
/// follow the neighbours (chord-length tangents, out = m/3, in = −m/3).
std::vector<Knot> fitKnots(std::span<const Dab> dabs, const std::function<std::uint64_t()>& nextId);

/// The stamps of an object replayed from its geometry (§8).
std::vector<Dab> objectDabs(const VectorObject& object);

/// Points of the flattened curve with their segment and parameter, for drawing and picking.
struct CurveSample {
    V point;
    std::size_t segment = 0;
    double t = 0.0;
    double width = 0.0;
};
std::vector<CurveSample> flatten(const VectorObject& object);

/// The nearest point of the curve to p (nullopt for an empty object).
std::optional<CurveSample> nearest(const VectorObject& object, V p, double* distance = nullptr);

/// Sets handle `out` (or `in`) of a knot and applies its join to the other one: symmetric gives −h,
/// smooth −normalize(h)·oldLength (unchanged when h is zero), corner leaves it.
void setHandle(Knot& knot, bool out, V handle);

/// Inserts a knot on segment `segment` at t without changing the shape (§8.1).
VectorObject insertKnot(const VectorObject& object, std::size_t segment, double t, std::uint64_t newId);

/// Removes knot `index`; the neighbours keep their remaining handles. nullopt when the object goes.
std::optional<VectorObject> removeKnot(const VectorObject& object, std::size_t index);

/// Checks an object read from a file (§8, §10.3); empty when valid.
std::string validateObject(const VectorObject& object);

}  // namespace chroma::core

#pragma once
// ChromaStudio Engine · core · input packets → stamps for one stroke (spec §3, §4).
//
//   packets (document px, ms) → late/same-time handling → stabiliser → equal-position merge
//   → StrokeWalker (§3.1) → dabs: r = 0.5·diameter(p)·f^radiusPower,
//                                o = opacityAt(p)·f^opacityPower, inkWeight = o·flow.
//
// While the pen moves, dabs use only the start taper f = F(s/I): the end of the path is unknown.
// Dabs of final segments are stable; the tail (provisional last segment) is recomputed each packet.
// On lift the real last position is appended, the lift velocity sets the end taper and, if the
// envelope changes any dab, the whole stroke is replayed from the base with the final envelope.
// The confirmed result is therefore exactly the full computation after Up (spec §3).

#include "core/BrushProfile.hpp"
#include "core/Geometry.hpp"
#include "core/Stabilizer.hpp"

#include <optional>
#include <vector>

namespace chroma::core {

/// One contact packet after the view transform.
struct StrokeSample {
    double timeMs = 0.0;
    V position;  // document px
    double pressure = 1.0;
    double tiltX = 0.0;  // radians
    double tiltY = 0.0;
};

/// One stamp of the §4.1 tip program.
struct Dab {
    V center;               // document px
    double radius = 0.0;    // major half-axis, document px
    double aspect = 1.0;    // minor / major
    double angle = 0.0;     // radians, document axes (y down)
    double inkWeight = 0.0;  // o · flow
    double arc = 0.0;       // arc length at the stamp
};

struct StrokeResult {
    std::vector<Dab> dabs;
    bool replay = false;  // true: dabs is the whole stroke, re-render from the base; false: append dabs
    double length = 0.0;
    double liftVelocity = 0.0;  // document px / s
};

class StrokeBuilder {
public:
    /// `diameterPx` is the effective size (the size slider overrides the profile, spec §6).
    StrokeBuilder(const BrushProfile& profile, double diameterPx);
    StrokeBuilder(const StrokeBuilder&) = delete;
    StrokeBuilder& operator=(const StrokeBuilder&) = delete;

    /// Down / Move packet. Appends the dabs that became final.
    void add(const StrokeSample& sample, std::vector<Dab>& finalDabs);

    /// Dabs of the provisional tail, replacing the contents of `tailDabs`.
    void tail(std::vector<Dab>& tailDabs);

    /// Up packet: completes the stroke.
    StrokeResult finish(const StrokeSample& lift);

    [[nodiscard]] const BrushProfile& profile() const noexcept { return profile_; }

private:
    struct Orientation {
        double tilt = 0.0;  // last non-zero tilt argument
        double path = 0.0;  // last non-zero tangent argument
        bool hasPosition = false;
        V lastPosition;
    };

    [[nodiscard]] Dab makeDab(const Mark& mark, double envelopeFactor, Orientation& orientation, bool touch) const;
    [[nodiscard]] double previewEnvelope(double arc) const;
    [[nodiscard]] double liftVelocity() const;
    void commitPending(std::vector<Dab>& out);
    void appendNode(const Node& node, std::vector<Dab>& out);

    BrushProfile profile_;
    Style style_;
    Stabilizer stabilizer_;
    StrokeWalker walker_;
    std::optional<Node> pending_;  // newest node, held back until a later time stamp confirms it
    double pendingMs_ = 0.0;
    std::vector<StrokeSample> raw_;  // contact polyline for the lift velocity and the real end
    Orientation orientation_;
};

}  // namespace chroma::core

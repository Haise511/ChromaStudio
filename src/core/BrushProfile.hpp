#pragma once
// ChromaStudio Engine · core · brush profiles (spec §6).
//
// Field names, ranges and family rules follow the §6.1 schema; builtInBrushes() is the §6.2
// starting catalogue. Diameters and lengths are document pixels, angles degrees, times ms.
// The JSON reader and the importer (§14) arrive with the brush library; this is the in-memory form.

#include "core/Geometry.hpp"
#include "core/Stabilizer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chroma::core {

enum class BrushFamily : std::uint8_t { Dip, Fade, Fountain, Airbrush, Watercolor, Texture, Screentone };
enum class TipOrientation : std::uint8_t { Fixed, Tilt, Path };
enum class Accumulate : std::uint8_t { Max, Build };
enum class TaperShape : std::uint8_t { Linear, Smooth };
/// §14.2: how a mask PNG becomes R8 — a, r·a, or (1 − 0.2126r − 0.7152g − 0.0722b)·a.
enum class MaskChannel : std::uint8_t { Alpha, RedAlpha, InverseLumaAlpha };

struct Tip {
    double diameter = 12.0;  // [0.25, 2048]
    double aspect = 1.0;     // [0.05, 1]
    double hardness = 1.0;   // [0, 1]
    double angleDeg = 0.0;   // [-180, 180]
    TipOrientation orientation = TipOrientation::Fixed;
    std::optional<std::string> maskId;  // SHA-256 of a local PNG (§6.1 extension); none: procedural
    MaskChannel maskChannel = MaskChannel::Alpha;
    friend bool operator==(const Tip&, const Tip&) = default;
};

struct Ink {
    double opacity = 1.0;
    double flow = 1.0;
    Accumulate accumulate = Accumulate::Max;
    double minSize = 0.0;
    double minOpacity = 0.0;
    double spacing = 0.1;  // [0.01, 2], fraction of the diameter
    int blendId = 0;       // §6.1 extension: the stroke's own §7 mode
    friend bool operator==(const Ink&, const Ink&) = default;
};

struct Taper {
    double startPx = 0.0;  // [0, 2048]
    double endPx = 0.0;    // [0, 2048]
    double liftGain = 0.0;  // [0, 3]
    TaperShape shape = TaperShape::Smooth;
    double radiusPower = 0.0;   // [0, 4]
    double opacityPower = 0.0;  // [0, 4]
    friend bool operator==(const Taper&, const Taper&) = default;
};

struct Wet {
    double blending = 0.0;
    double dilution = 0.0;
    double bleed = 0.0;
    double strength = 1.0;
    friend bool operator==(const Wet&, const Wet&) = default;
};

struct Grain {
    double grainPx = 1.0;      // [0.25, 512]
    double grainAmount = 0.0;  // [0, 1]
    std::uint32_t seed = 0;
    std::optional<std::string> maskId;  // SHA-256 of a local PNG tiled over the canvas (§4)
    friend bool operator==(const Grain&, const Grain&) = default;
};

struct Tone {
    double periodPx = 8.0;  // [2, 256]
    double density = 0.0;   // [0, 1]
    double toneAngleDeg = 0.0;
    friend bool operator==(const Tone&, const Tone&) = default;
};

struct BrushProfile {
    std::string id;    // ^[a-z][a-z0-9.-]{0,63}$
    std::string name;  // 1..80 characters, UTF-8
    BrushFamily family = BrushFamily::Dip;
    Tip tip;
    Ink ink;
    StabilizerSettings stabilizer;
    Taper taper;
    std::optional<Wet> wet;      // watercolor only
    std::optional<Grain> grain;  // texture only
    std::optional<Tone> tone;    // screentone only
    std::vector<PressurePoint> sizeCurve;     // optional "pressure.size"
    std::vector<PressurePoint> opacityCurve;  // optional "pressure.opacity"
    friend bool operator==(const BrushProfile&, const BrushProfile&) = default;
};

/// Throws std::invalid_argument naming the first field outside the §6.1 contract.
void validate(const BrushProfile& profile);

/// §6.2: dip, fade, fountain, airbrush, watercolor, texture, screentone.
const std::vector<BrushProfile>& builtInBrushes();

/// Tip program kind of §4.1: 0 tip, 1 airbrush, 2 texture, 3 screentone.
int tipKind(const BrushProfile& profile) noexcept;

/// Wet families need the §5 pass.
inline bool isDry(const BrushProfile& profile) noexcept { return profile.family != BrushFamily::Watercolor; }

/// The record uses fields beyond version 1.0 (§6: pressure, ink.blendId, tip.maskId/maskChannel), which
/// projects announce with minor = 1 and requiredFeatures ["brushDynamics1"] (§14).
bool usesDynamics1(const BrushProfile& profile) noexcept;

/// True for a lowercase SHA-256 in hex.
bool isSha256(const std::string& text) noexcept;

}  // namespace chroma::core

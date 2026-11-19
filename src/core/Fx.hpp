#pragma once
// ChromaStudio Engine · core · image filters: parameters, ranges and the tone-curve table (spec §9).
//
// Every parameter is checked for range on the CPU before it reaches a shader (§9: "Все uniform-
// параметры сначала проверяются на конечность и диапазон на CPU"). Halos are the exact read margins
// of §9.2–§9.6. The tone curve has Master, R, G, B with 2–32 points each, x strictly increasing from 0
// to 1, linear interpolation, applied as C_j(Master(x)) through a 256 × 1 RGBA table.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace chroma::core {

enum class FxKind : std::uint8_t { Gaussian, ToneCurve, Unsharp, Chromatic, AnimeLines };
inline constexpr int kFxCount = 5;

struct CurvePoint {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

/// Channels: 0 Master, 1 R, 2 G, 3 B.
struct ToneCurve {
    std::array<std::vector<CurvePoint>, 4> channels{{{{0, 0}, {1, 1}}, {{0, 0}, {1, 1}}, {{0, 0}, {1, 1}}, {{0, 0}, {1, 1}}}};
    bool discrete = false;
    friend bool operator==(const ToneCurve&, const ToneCurve&) = default;
};

struct FxSettings {
    FxKind kind = FxKind::Gaussian;
    int radius = 6;         // Gaussian [0, 32] (6), Unsharp [0, 32] (3), AnimeLines [0, 8] (0)
    ToneCurve curve;        // ToneCurve
    double gain = 1.0;      // Unsharp [0, 4]
    double threshold = 0.01;  // Unsharp [0, 1], linear colour
    double shiftX = 1.0;    // Chromatic [-32, 32]
    double shiftY = 0.0;
    double blackPoint = 0.08;  // AnimeLines, gap ≥ 1/255 to whitePoint
    double whitePoint = 0.92;
    double mid = 0.5;          // [0.01, 0.99]
    double edgeGain = 0.0;     // [0, 4]
    bool toNewLayer = true;    // AnimeLines: a separate raster with the extracted alpha
    double amount = 1.0;       // the final mix(original, filtered, selection·amount), [0, 1]
    friend bool operator==(const FxSettings&, const FxSettings&) = default;
};

/// The §9 starting values of one filter.
FxSettings defaultFx(FxKind kind);
std::string_view fxName(FxKind kind) noexcept;

/// Clamps every parameter into its range (NaN becomes the default) and repairs the curve rules.
FxSettings sanitize(const FxSettings& settings);

/// Read margin around an output pixel, in document pixels (§9.2–§9.6).
int fxHalo(const FxSettings& settings);

/// True when the filter can spread colour into transparent pixels (so neighbouring empty tiles are
/// processed too): the blur, and the edges of the line extraction.
bool fxSpreads(const FxSettings& settings);

/// §9.3: 2–32 points, x strictly increasing, the end x are 0 and 1, y in [0, 1].
bool validCurve(const std::vector<CurvePoint>& points);
/// Linear interpolation between the points (x clamped to [0, 1]).
double evalCurve(const std::vector<CurvePoint>& points, double x);
/// §9.3: L[i].rgb = (C_r, C_g, C_b)(Master(i/255)), alpha 1; 256 × RGBA floats.
std::array<float, 256 * 4> curveTable(const ToneCurve& curve);

}  // namespace chroma::core

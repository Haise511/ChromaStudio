#include "core/BrushProfile.hpp"

#include <algorithm>

#include <cmath>
#include <stdexcept>
#include <string_view>

namespace chroma::core {
namespace {

void require(bool ok, std::string_view field) {
    if (!ok) throw std::invalid_argument("brush field out of range: " + std::string(field));
}

bool inRange(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool validId(std::string_view id) {
    if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z') return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::size_t codePoints(std::string_view utf8) {
    std::size_t count = 0;
    for (const char c : utf8) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) ++count;  // not a continuation byte
    }
    return count;
}

BrushProfile make(std::string id, std::string name, BrushFamily family, Tip tip, Ink ink, StabilizerSettings stabilizer,
                  Taper taper) {
    BrushProfile p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.family = family;
    p.tip = tip;
    p.ink = ink;
    p.stabilizer = stabilizer;
    p.taper = taper;
    return p;
}

std::vector<BrushProfile> makeCatalog() {
    using enum BrushFamily;
    constexpr auto kFixed = TipOrientation::Fixed;
    constexpr auto kTilt = TipOrientation::Tilt;
    constexpr auto kPath = TipOrientation::Path;
    constexpr auto kMaxInk = Accumulate::Max;
    constexpr auto kBuildInk = Accumulate::Build;
    constexpr auto kSmooth = TaperShape::Smooth;
    constexpr auto kOff = StabilizerMethod::Off;
    constexpr auto kEma = StabilizerMethod::Ema;
    constexpr auto kWindow = StabilizerMethod::Window;

    std::vector<BrushProfile> catalog;
    catalog.push_back(make("core.dip", "Твёрдое перо", Dip, {12, 1, 1, 0, kFixed}, {1, 1, kMaxInk, 0.1, 0.85, 0.08},
                           {kEma, 10}, {0, 6, 0.5, kSmooth, 1, 0}));
    catalog.push_back(make("core.fade", "Мягкое перо", Fade, {18, 1, 0.6, 0, kFixed},
                           {0.95, 1, kMaxInk, 0.15, 0.1, 0.08}, {kEma, 14}, {8, 24, 1, kSmooth, 1, 1}));
    catalog.push_back(make("core.fountain", "Перьевая ручка", Fountain, {16, 0.32, 0.95, 35, kTilt},
                           {1, 1, kMaxInk, 0.3, 0.8, 0.055}, {kWindow, 16}, {2, 8, 0.5, kSmooth, 1, 0}));
    catalog.push_back(make("core.airbrush", "Аэрограф", Airbrush, {96, 1, 0, 0, kFixed},
                           {0.7, 0.12, kBuildInk, 0.6, 0.2, 0.12}, {kOff, 0}, {0, 0, 0, kSmooth, 0, 0}));

    BrushProfile watercolor = make("core.watercolor", "Акварель", Watercolor, {64, 0.86, 0.45, 0, kPath},
                                   {0.78, 0.42, kBuildInk, 0.28, 0.35, 0.12}, {kEma, 18},
                                   {0, 10, 0.4, kSmooth, 1, 0.5});
    watercolor.wet = Wet{0.72, 0.22, 0.65, 1};
    catalog.push_back(std::move(watercolor));

    BrushProfile texture = make("core.texture", "Зернистая кисть", Texture, {40, 0.7, 0.8, 12, kPath},
                                {0.92, 0.8, kBuildInk, 0.25, 0.25, 0.16}, {kEma, 8}, {0, 4, 0, kSmooth, 1, 0});
    texture.grain = Grain{2.5, 0.85, 1847};
    catalog.push_back(std::move(texture));

    BrushProfile screentone = make("core.screentone", "Точечный скринтон", Screentone, {72, 1, 1, 0, kFixed},
                                   {1, 1, kMaxInk, 1, 1, 0.1}, {kOff, 0}, {0, 0, 0, kSmooth, 0, 0});
    screentone.tone = Tone{8, 0.35, 45};
    catalog.push_back(std::move(screentone));

    for (const BrushProfile& p : catalog) validate(p);
    return catalog;
}

}  // namespace

void validate(const BrushProfile& p) {
    require(validId(p.id), "id");
    require(codePoints(p.name) >= 1 && codePoints(p.name) <= 80, "name");

    require(inRange(p.tip.diameter, 0.25, 2048), "tip.diameter");
    require(inRange(p.tip.aspect, 0.05, 1), "tip.aspect");
    require(inRange(p.tip.hardness, 0, 1), "tip.hardness");
    require(inRange(p.tip.angleDeg, -180, 180), "tip.angleDeg");

    require(inRange(p.ink.opacity, 0, 1), "ink.opacity");
    require(inRange(p.ink.flow, 0, 1), "ink.flow");
    require(inRange(p.ink.minSize, 0, 1), "ink.minSize");
    require(inRange(p.ink.minOpacity, 0, 1), "ink.minOpacity");
    require(inRange(p.ink.spacing, 0.01, 2), "ink.spacing");
    require(p.ink.blendId >= 0 && p.ink.blendId <= 26, "ink.blendId");
    require(!p.tip.maskId || isSha256(*p.tip.maskId), "tip.maskId");
    require(!p.grain || !p.grain->maskId || isSha256(*p.grain->maskId), "grain.maskId");

    require(inRange(p.stabilizer.timeMs, 0, 200), "stabilizer.timeMs");

    require(inRange(p.taper.startPx, 0, 2048), "taper.startPx");
    require(inRange(p.taper.endPx, 0, 2048), "taper.endPx");
    require(inRange(p.taper.liftGain, 0, 3), "taper.liftGain");
    require(inRange(p.taper.radiusPower, 0, 4), "taper.radiusPower");
    require(inRange(p.taper.opacityPower, 0, 4), "taper.opacityPower");

    validateCurve(p.sizeCurve);
    validateCurve(p.opacityCurve);

    // Family rules (§6.1 allOf).
    require((p.family == BrushFamily::Watercolor) == p.wet.has_value(), "wet");
    require((p.family == BrushFamily::Texture) == p.grain.has_value(), "grain");
    require((p.family == BrushFamily::Screentone) == p.tone.has_value(), "tone");
    if (p.family == BrushFamily::Watercolor) require(p.ink.accumulate == Accumulate::Build, "ink.accumulate");
    if (p.wet) {
        require(inRange(p.wet->blending, 0, 1), "wet.blending");
        require(inRange(p.wet->dilution, 0, 1), "wet.dilution");
        require(inRange(p.wet->bleed, 0, 1), "wet.bleed");
        require(inRange(p.wet->strength, 0, 1), "wet.strength");
    }
    if (p.grain) {
        require(inRange(p.grain->grainPx, 0.25, 512), "grain.grainPx");
        require(inRange(p.grain->grainAmount, 0, 1), "grain.grainAmount");
    }
    if (p.tone) {
        require(inRange(p.tone->periodPx, 2, 256), "tone.periodPx");
        require(inRange(p.tone->density, 0, 1), "tone.density");
        require(inRange(p.tone->toneAngleDeg, -180, 180), "tone.toneAngle");
    }
}

bool isSha256(const std::string& text) noexcept {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool usesDynamics1(const BrushProfile& p) noexcept {
    return !p.sizeCurve.empty() || !p.opacityCurve.empty() || p.ink.blendId != 0 || p.tip.maskId.has_value();
}

const std::vector<BrushProfile>& builtInBrushes() {
    static const std::vector<BrushProfile> catalog = makeCatalog();
    return catalog;
}

int tipKind(const BrushProfile& profile) noexcept {
    switch (profile.family) {
    case BrushFamily::Airbrush: return 1;
    case BrushFamily::Texture: return 2;
    case BrushFamily::Screentone: return 3;
    default: return 0;
    }
}

}  // namespace chroma::core

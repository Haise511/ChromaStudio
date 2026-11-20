// ChromaStudio Engine · engine_tests · core (no window, no GPU).
// Numerical checks of spec §12.1 plus the contracts of §3, §6, §10.2 and §2 that the core implements.

#include "TestHarness.hpp"

#include "core/BrushProfile.hpp"
#include "core/Color.hpp"
#include "core/Compositor.hpp"
#include "core/Fx.hpp"
#include "core/Geometry.hpp"
#include "core/Half.hpp"
#include "core/LayerTree.hpp"
#include "core/RasterOps.hpp"
#include "core/Selection.hpp"
#include "core/Document.hpp"
#include "core/Vector.hpp"
#include "core/Stabilizer.hpp"
#include "core/TileData.hpp"
#include "core/StrokeBuilder.hpp"
#include "core/TileHistory.hpp"
#include "core/View.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <memory>
#include <numbers>
#include <set>
#include <vector>

using namespace chroma::core;
namespace core = chroma::core;

namespace {

std::vector<Mark> walkMarks(const std::vector<Node>& nodes, const Style& style) {
    std::vector<Mark> marks;
    walk(nodes, style, [&](const Mark& m) { marks.push_back(m); });
    return marks;
}

const BrushProfile& brush(std::string_view id) {
    for (const BrushProfile& p : builtInBrushes()) {
        if (p.id == id) return p;
    }
    throw std::runtime_error("unknown brush");
}

}  // namespace

// §12.1 "Шаг и касание": x = 0 → 10, D = 12, spacing = .25, minSize = 1 → 0, 3, 6, 9, 10.
TEST_CASE(stepAndTouch) {
    const Style style{12.0, 1.0, 0.25};
    for (const std::vector<Node>& nodes :
         {std::vector<Node>{{{0, 0}}, {{10, 0}}}, std::vector<Node>{{{0, 0}}, {{5, 0}}, {{10, 0}}}}) {
        const std::vector<Mark> marks = walkMarks(nodes, style);
        const double expected[] = {0, 3, 6, 9, 10};
        CHECK(marks.size() == 5);
        for (std::size_t i = 0; i < marks.size() && i < 5; ++i) {
            CHECK_NEAR(marks[i].arc, expected[i], 1e-9);
            CHECK_NEAR(marks[i].node.xy.x, expected[i], 1e-9);
        }
    }
    const std::vector<Mark> touch = walkMarks({{{4, 4}}}, style);
    CHECK(touch.size() == 1);
    // A path shorter than one step keeps both ends.
    const std::vector<Mark> shortPath = walkMarks({{{0, 0}}, {{1, 0}}}, style);
    CHECK(shortPath.size() == 2);
    if (shortPath.size() == 2) CHECK_NEAR(shortPath[1].arc, 1.0, 1e-9);
}

// The incremental walker gives exactly the batch walk, and previewing the tail changes nothing.
TEST_CASE(incrementalWalkMatchesBatch) {
    const Style style{9.0, 0.2, 0.1};
    std::vector<Node> nodes;
    for (int i = 0; i < 40; ++i) {
        const double t = i * 0.37;
        nodes.push_back({{20 * t, 30 * std::sin(t)}, 0.3 + 0.7 * std::abs(std::sin(1.7 * t)), 0.1 * t, -0.05 * t});
    }
    const std::vector<Mark> batch = walkMarks(nodes, style);

    std::vector<Mark> incremental;
    StrokeWalker walker(style);
    const auto sink = [&](const Mark& m) { incremental.push_back(m); };
    for (const Node& n : nodes) {
        walker.append(n, sink);
        std::size_t previewCount = 0;
        walker.previewTail(nullptr, [&](const Mark&) { ++previewCount; });
        (void)previewCount;
    }
    walker.finish(sink);

    CHECK(batch.size() == incremental.size());
    for (std::size_t i = 0; i < batch.size() && i < incremental.size(); ++i) {
        CHECK_NEAR(incremental[i].arc, batch[i].arc, 1e-12);
        CHECK_NEAR(incremental[i].node.xy.x, batch[i].node.xy.x, 1e-12);
        CHECK_NEAR(incremental[i].node.xy.y, batch[i].node.xy.y, 1e-12);
        CHECK_NEAR(incremental[i].node.pressure, batch[i].node.pressure, 1e-12);
        CHECK_NEAR(incremental[i].node.tiltX, batch[i].node.tiltX, 1e-12);
    }
    // Consecutive marks are never further apart (along the curve) than one full step.
    for (std::size_t i = 1; i < batch.size(); ++i) CHECK(batch[i].arc - batch[i - 1].arc <= 9.0 * 0.1 + 1e-9);
}

TEST_CASE(invalidGeometryRejected) {
    CHECK_THROWS(walkMarks({{{0, 0}}}, Style{0.1, 0, 0.1}));                 // diameter below 0.25
    CHECK_THROWS(walkMarks({{{0, 0}}}, Style{12, 0, 3}));                    // spacing above 2
    CHECK_THROWS(walkMarks({{{0, 0}, 1.5}}, Style{12, 0, 0.1}));             // pressure above 1
    CHECK_THROWS(walkMarks({{{200000, 0}}}, Style{12, 0, 0.1}));             // outside the workspace
    const PressurePoint badCurve[] = {{0.2, 0.0}, {1.0, 1.0}};
    CHECK_THROWS(validateCurve(badCurve));
}

// §12.1 "EMA и окно": step 0 → 100 with dt = τ = 10 ms gives 63.212056; two 5 ms steps agree.
TEST_CASE(emaStep) {
    const V once = ema({0, 0}, {100, 0}, 10, 10);
    CHECK_NEAR(once.x, 63.212056, 1e-5);
    const V twice = ema(ema({0, 0}, {100, 0}, 5, 10), {100, 0}, 5, 10);
    CHECK_NEAR(twice.x, once.x, 1e-9);
    CHECK_NEAR(ema({0, 0}, {100, 0}, 300, 10).x, 100.0, 0.0);  // idle > 250 ms re-initialises
    CHECK_NEAR(ema({0, 0}, {100, 0}, 10, 0).x, 100.0, 0.0);    // τ = 0 passes the input
}

// §12.1: a 10 ms window on x = t with samples at t = 0, 7, 14 averages [4, 14] to 9.
TEST_CASE(windowMeanLinear) {
    const TimedXY samples[] = {{0, 0, 0}, {7, 7, 0}, {14, 14, 0}};
    CHECK_NEAR(windowMean(samples, 10)[0], 9.0, 1e-12);
    CHECK_NEAR(windowMean(samples, 0)[0], 14.0, 0.0);
    CHECK_THROWS(windowMean(samples, 250));
}

TEST_CASE(stabilizerContracts) {
    Stabilizer emaFilter({StabilizerMethod::Ema, 10});
    CHECK_NEAR(emaFilter.push(0, {0, 0}).x, 0.0, 0.0);
    CHECK_NEAR(emaFilter.push(10, {100, 0}).x, 63.212056, 1e-5);
    // Same time: the last position wins, as if the previous sample never existed.
    CHECK_NEAR(emaFilter.push(10, {50, 0}).x, 31.606028, 1e-5);
    // A late sample changes nothing.
    CHECK_NEAR(emaFilter.push(5, {1000, 0}).x, 31.606028, 1e-5);
    // A pause over 250 ms re-initialises at the current position.
    CHECK_NEAR(emaFilter.push(300, {7, 0}).x, 7.0, 0.0);

    Stabilizer windowFilter({StabilizerMethod::Window, 10});
    windowFilter.push(0, {0, 0});
    windowFilter.push(7, {7, 0});
    CHECK_NEAR(windowFilter.push(14, {14, 0}).x, 9.0, 1e-12);

    Stabilizer off({StabilizerMethod::Off, 0});
    off.push(0, {1, 2});
    CHECK_NEAR(off.push(1, {5, 6}).y, 6.0, 0.0);
    CHECK_THROWS(Stabilizer({StabilizerMethod::Ema, 500}));
}

// §12.1 "Сужение": L = 100, I = 10, E = 20, liftGain = 1, v = 1200, linear → effective E = 40.
TEST_CASE(taperEnvelope) {
    CHECK_NEAR(envelope(5, 100, 10, 20, 1200, 1, true), 0.5, 1e-12);
    CHECK_NEAR(envelope(80, 100, 10, 20, 1200, 1, true), 0.5, 1e-12);
    CHECK_NEAR(envelope(100, 100, 10, 20, 1200, 1, true), 0.0, 1e-12);
    CHECK_NEAR(envelope(50, 100, 0, 0, 1200, 1, true), 1.0, 0.0);  // taper off: visible everywhere
    CHECK_NEAR(envelope(0, 0, 10, 20, 0, 0, false), 1.0, 0.0);    // a touch has no length
}

TEST_CASE(pressureMapping) {
    const Style style{12, 0.1, 0.1};
    CHECK_NEAR(diameter(style, 1.0), 12.0, 1e-12);
    CHECK_NEAR(diameter(style, 0.0), 1.2, 1e-12);
    CHECK_NEAR(diameter(style, 0.5), 12 * (0.1 + 0.9 * 0.5), 1e-12);  // q = p²(3 − 2p) = .5 at p = .5
    const PressurePoint curve[] = {{0, 0}, {0.5, 1}, {1, 1}};
    CHECK_NEAR(pressureGain(0.25, curve), 0.5, 1e-12);
    CHECK_NEAR(opacityAt(style, 0.0, 0.8, 0.25), 0.2, 1e-12);
}

// A single touch gives one full stamp (§3).
TEST_CASE(strokeTap) {
    StrokeBuilder builder(brush("core.dip"), 12);
    std::vector<Dab> dabs;
    builder.add({0, {100, 100}, 1.0}, dabs);
    const StrokeResult result = builder.finish({10, {100, 100}, 1.0});
    CHECK(result.replay);
    CHECK(result.length == 0.0);
    CHECK(result.dabs.size() == 1);
    if (!result.dabs.empty()) {
        CHECK_NEAR(result.dabs[0].radius, 6.0, 1e-12);
        CHECK_NEAR(result.dabs[0].inkWeight, 1.0, 1e-12);
        CHECK_NEAR(result.dabs[0].center.x, 100.0, 1e-12);
    }
}

// Dip pen: grows from nothing over its first 7 px (as ibisPaint's dip pen), end tapered to zero at the
// real last position; the replay covers the stroke.
TEST_CASE(strokeLineWithEndTaper) {
    StrokeBuilder builder(brush("core.dip"), 12);
    std::vector<Dab> finals;
    std::vector<Dab> tail;
    for (int i = 0; i <= 100; ++i) {
        builder.add({static_cast<double>(i), {static_cast<double>(i), 50}, 1.0}, finals);
        builder.tail(tail);
    }
    CHECK(!finals.empty());
    const StrokeResult result = builder.finish({101, {100, 50}, 1.0});
    CHECK(result.replay);
    // Uniform Catmull-Rom over the unevenly spaced filtered nodes may bulge a little past the chord.
    CHECK(result.length >= 100.0 - 1e-9 && result.length < 103.0);
    // §3: raw contact length over the last 30 ms (29 px: the final millisecond is the lift at rest) / 30 ms.
    CHECK_NEAR(result.liftVelocity, 29.0 / 30.0 * 1000.0, 1e-6);
    CHECK(result.dabs.size() > 50);
    if (result.dabs.size() > 2) {
        CHECK_NEAR(result.dabs.front().radius, 0.0, 1e-9);
        for (const Dab& d : result.dabs) {
            if (d.arc >= 7.0 && d.arc <= 40.0) CHECK_NEAR(d.radius, 6.0, 1e-9);  // full width from 7 px on
            if (d.arc > 0.5 && d.arc < 6.5) CHECK(d.radius > 0.0 && d.radius < 6.0);
        }
        CHECK_NEAR(result.dabs.back().center.x, 100.0, 1e-9);  // the real end, not the filtered lag
        CHECK_NEAR(result.dabs.back().radius, 0.0, 1e-9);
        for (std::size_t i = 1; i < result.dabs.size(); ++i) CHECK(result.dabs[i].arc > result.dabs[i - 1].arc);
    }
}

// Airbrush has no taper powers: the incremental dabs are final, spaced exactly one step apart.
TEST_CASE(strokeWithoutReplay) {
    StrokeBuilder builder(brush("core.airbrush"), 96);
    std::vector<Dab> dabs;
    for (int i = 0; i <= 200; i += 4) builder.add({static_cast<double>(i), {static_cast<double>(i), 0}, 1.0}, dabs);
    const StrokeResult result = builder.finish({201, {200, 0}, 1.0});
    CHECK(!result.replay);
    dabs.insert(dabs.end(), result.dabs.begin(), result.dabs.end());
    const double step = 96 * 0.12;
    CHECK(dabs.size() == static_cast<std::size_t>(std::floor(200 / step)) + 2);
    for (std::size_t i = 1; i + 1 < dabs.size(); ++i) CHECK_NEAR(dabs[i].center.x - dabs[i - 1].center.x, step, 1e-9);
    CHECK_NEAR(dabs.back().center.x, 200.0, 1e-9);
}

// Short path with both tapers: every regular stamp has f = 0, so the apex stamp is added (§3).
TEST_CASE(strokeShortApex) {
    StrokeBuilder builder(brush("core.fade"), 18);
    std::vector<Dab> dabs;
    builder.add({0, {10, 10}, 1.0}, dabs);
    builder.add({50, {12, 10}, 1.0}, dabs);
    const StrokeResult result = builder.finish({60, {12, 10}, 1.0});
    CHECK(result.replay);
    bool visible = false;
    for (const Dab& d : result.dabs) visible = visible || (d.radius > 0 && d.inkWeight > 0);
    CHECK(visible);
}

// The size slider scales the tapers with the brush: at 3 px the dip pen is full width after 1.75 px.
TEST_CASE(taperScalesWithSize) {
    StrokeBuilder builder(brush("core.dip"), 3);
    std::vector<Dab> finals;
    for (int i = 0; i <= 60; ++i) builder.add({static_cast<double>(i), {static_cast<double>(i), 50}, 1.0}, finals);
    const StrokeResult result = builder.finish({61, {60, 50}, 1.0});
    bool sawRamp = false;
    for (const Dab& d : result.dabs) {
        if (d.arc >= 1.75 && d.arc <= 20.0) CHECK_NEAR(d.radius, 1.5, 1e-9);
        if (d.arc > 0.2 && d.arc < 1.6) sawRamp = sawRamp || (d.radius > 0.0 && d.radius < 1.5);
    }
    CHECK(sawRamp);
    // A tap is still one full stamp.
    StrokeBuilder tap(brush("core.dip"), 3);
    std::vector<Dab> none;
    tap.add({0, {10, 10}, 1.0}, none);
    const StrokeResult dot = tap.finish({5, {10, 10}, 1.0});
    CHECK(dot.dabs.size() == 1 && !dot.dabs.empty() && std::abs(dot.dabs[0].radius - 1.5) < 1e-12);
}

// The hard pen answers pressure as ibisPaint's dip pen, measured with the same synthetic pen: width share
// of the full size at 2 … 100 % pressure, opacity independent of pressure.
TEST_CASE(hardPenPressureMatchesIbisPaint) {
    const BrushProfile& pen = brush("core.dip");
    const Style style{pen.tip.diameter, pen.ink.minSize, pen.ink.spacing, pen.sizeCurve, pen.opacityCurve};
    const std::pair<double, double> measured[] = {{0.02, 0.0526}, {0.05, 0.0693}, {0.1, 0.1013}, {0.2, 0.1680},
                                                  {0.3, 0.2298}, {0.5, 0.3822}, {0.7, 0.5847}, {1.0, 1.0}};
    for (const auto& [pressure, share] : measured) {
        CHECK_NEAR(diameter(style, pressure) / pen.tip.diameter, share, 0.01);
        CHECK_NEAR(opacityAt(style, pressure, pen.ink.opacity, pen.ink.minOpacity), 1.0, 1e-12);
    }
    CHECK(diameter(style, 0.0) / pen.tip.diameter < 0.05);  // a hairline at the lightest touch
}

TEST_CASE(fountainFollowsTilt) {
    StrokeBuilder builder(brush("core.fountain"), 16);
    std::vector<Dab> dabs;
    for (int i = 0; i <= 60; ++i) {
        builder.add({static_cast<double>(i), {static_cast<double>(i), 0}, 1.0, 0.0, 0.5}, dabs);  // tilt toward +y
    }
    CHECK(!dabs.empty());
    const double expected = 35 * std::numbers::pi / 180 + std::numbers::pi / 2;
    if (!dabs.empty()) CHECK_NEAR(dabs.back().angle, expected, 1e-9);
}

TEST_CASE(brushCatalog) {
    const auto& catalog = builtInBrushes();
    CHECK(catalog.size() == 7);
    std::set<std::string> ids;
    for (const BrushProfile& p : catalog) {
        validate(p);
        ids.insert(p.id);
    }
    CHECK(ids.size() == 7);
    CHECK(tipKind(brush("core.airbrush")) == 1);
    CHECK(tipKind(brush("core.texture")) == 2);
    CHECK(tipKind(brush("core.screentone")) == 3);
    CHECK(!isDry(brush("core.watercolor")));

    BrushProfile tooSmall = brush("core.dip");
    tooSmall.tip.diameter = 0.1;  // §12.3: a profile with a wrong diameter is rejected
    CHECK_THROWS(validate(tooSmall));
    BrushProfile noGrain = brush("core.texture");
    noGrain.grain.reset();  // a required material is missing
    CHECK_THROWS(validate(noGrain));
    BrushProfile badId = brush("core.dip");
    badId.id = "Core Dip";
    CHECK_THROWS(validate(badId));
}

// §10.2 cursor semantics with a stand-in tile handle.
TEST_CASE(tileHistory) {
    using Ref = std::shared_ptr<int>;
    TileHistory<Ref> history(1 << 20, 1);
    const TileKey k{1, 0, 0};
    const Ref a = std::make_shared<int>(1), b = std::make_shared<int>(2), c = std::make_shared<int>(3);

    CHECK(!history.undo());
    CHECK(!history.redo());
    CHECK(history.commit({{k, nullptr, a}}));
    CHECK(history.commit({{k, a, b}}));
    CHECK(history.get(k) == b);
    CHECK(history.undo());
    CHECK(history.get(k) == a);
    CHECK(history.undo());
    CHECK(history.get(k) == nullptr);
    CHECK(history.tileCount() == 0);
    CHECK(!history.undo());
    CHECK(history.redo());
    CHECK(history.get(k) == a);

    // An empty change does not cut the redo branch.
    CHECK(!history.commit({{k, a, a}}));
    CHECK(history.redo());
    CHECK(history.get(k) == b);

    // A new command after undo cuts the future only once it commits.
    CHECK(history.undo());
    CHECK(history.commit({{k, a, c}}));
    CHECK(history.size() == 2);
    CHECK(!history.redo());
    CHECK(history.get(k) == c);

    // Over budget, whole oldest commands go; the newest stays.
    TileHistory<Ref> small(3, 1);
    small.commit({{k, nullptr, a}});  // 1 reference
    small.commit({{k, a, b}});        // 2 references → 3 in total, at the budget
    CHECK(small.size() == 2);
    small.commit({{k, b, c}});        // 5 → oldest commands go until 2 ≤ 3; only the newest remains
    CHECK(small.size() == 1);
    CHECK(small.cursor() == 1);
    CHECK(small.undo());
    CHECK(small.get(k) == b);
    CHECK(!small.undo());
}

TEST_CASE(viewTransform) {
    ViewTransform view;
    view.fit({1000, 800}, {2000, 1000}, 1.0);
    CHECK_NEAR(view.zoom, 0.5, 1e-12);
    CHECK_NEAR(view.toView({1000, 500}).x, 500.0, 1e-12);  // centred
    const V anchor{321, 123};
    const V before = view.toDocument(anchor);
    view.zoomAround(anchor, 3.7);
    const V after = view.toDocument(anchor);
    CHECK_NEAR(after.x, before.x, 1e-9);
    CHECK_NEAR(after.y, before.y, 1e-9);
    view.zoomAround(anchor, 1e6);
    CHECK_NEAR(view.zoom, ViewTransform::kMaxZoom, 0.0);
    const V doc{17.25, -3.5};
    CHECK_NEAR(view.toDocument(view.toView(doc)).x, doc.x, 1e-9);
    view.actualPixels({1000, 800}, {2000, 1000}, 144);
    CHECK_NEAR(view.zoom, 96.0 / 144.0, 1e-12);  // one physical pixel per document pixel
}

TEST_CASE(srgbTransfer) {
    CHECK_NEAR(srgbToLinear(0.5), 0.21404114, 1e-7);
    CHECK_NEAR(srgbToLinear(0.04045), 0.04045 / 12.92, 1e-15);
    for (const double e : {0.0, 0.01, 0.2, 0.5, 0.8, 1.0}) CHECK_NEAR(linearToSrgb(srgbToLinear(e)), e, 1e-12);
    // The canvas ink: encoded sRGB times alpha, as painting editors blend (50 % black over white is 128).
    const auto ink = premultipliedInk(1.0, 0.0, 0.5, 0.5);
    CHECK(ink[0] == 0.5f && ink[1] == 0.0f && ink[2] == 0.25f && ink[3] == 0.5f);
    const auto clamped = premultipliedInk(1.5, -1.0, 0.5, 2.0);
    CHECK(clamped[0] == 1.0f && clamped[1] == 0.0f && clamped[2] == 0.5f && clamped[3] == 1.0f);
    // 0.1.0 files: linear premultiplied → the same colour as canvas ink.
    const auto migrated = linearPmaToInk(0.5 * 0.21404114, 0.0, 0.5, 0.5);
    CHECK_NEAR(migrated[0], 0.25, 1e-6);
    CHECK_NEAR(migrated[2], 0.5, 1e-6);
    CHECK(migrated[3] == 0.5f);
    const auto empty = linearPmaToInk(0.3, 0.2, 0.1, 0.0);
    CHECK(empty[0] == 0.0f && empty[3] == 0.0f);
    const auto p = premultipliedLinear(1.0, 0.0, 0.5, 0.5);
    CHECK_NEAR(p[0], 0.5, 1e-7);
    CHECK_NEAR(p[2], 0.5 * 0.21404114, 1e-6);
    CHECK_NEAR(p[3], 0.5, 0.0);
}

// §2: binary16 with round-to-nearest-even.
TEST_CASE(binary16Conversion) {
    CHECK(floatToHalf(1.0f) == 0x3C00);
    CHECK(floatToHalf(0.5f) == 0x3800);
    CHECK(floatToHalf(-2.0f) == 0xC000);
    CHECK(floatToHalf(65504.0f) == 0x7BFF);
    CHECK(floatToHalf(65520.0f) == 0x7C00);              // rounds up to infinity
    CHECK(floatToHalf(std::ldexp(1.0f, -24)) == 0x0001);  // smallest subnormal
    CHECK(floatToHalf(std::ldexp(1.0f, -26)) == 0x0000);  // below half of it: zero
    CHECK(floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);      // tie → even
    CHECK(floatToHalf(1.0f + 3 * std::ldexp(1.0f, -11)) == 0x3C02);  // tie → even (up)
    CHECK(floatToHalf(-0.0f) == 0x8000);
    CHECK(std::isnan(halfToFloat(floatToHalf(std::numeric_limits<float>::quiet_NaN()))));
    // Every half survives half → float → half (NaN payloads aside).
    bool exact = true;
    for (std::uint32_t h = 0; h <= 0xFFFF; ++h) {
        const auto half = static_cast<std::uint16_t>(h);
        const float f = halfToFloat(half);
        if (!std::isnan(f) && floatToHalf(f) != half) exact = false;
    }
    CHECK(exact);
}

TEST_CASE(tileCanonicalForm) {
    TileData tile;
    const auto set = [&](int x, int y, float r, float g, float b, float a) {
        std::uint16_t* p = &tile.halfs[(static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4];
        p[0] = floatToHalf(r);
        p[1] = floatToHalf(g);
        p[2] = floatToHalf(b);
        p[3] = floatToHalf(a);
    };
    const auto get = [&](int x, int y, int c) {
        return halfToFloat(tile.halfs[(static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4 + c]);
    };
    CHECK(isTransparent(tile));
    set(0, 0, 0.3f, 0.0f, 0.0f, 0.0f);    // alpha zero with colour
    set(1, 0, 0.6f, -0.0f, 0.2f, 0.5f);   // rgb above a, −0
    set(2, 0, 0.1f, 0.1f, 0.1f, 1.25f);   // alpha above 1
    set(100, 0, 0.5f, 0.5f, 0.5f, 1.0f);  // outside a 64-px-wide extent
    const TileExtent extent{64, kTileSize};
    CHECK(!validateTile(tile, extent).empty());
    CHECK(canonicalize(tile, extent));
    CHECK(validateTile(tile, extent).empty());
    CHECK(get(0, 0, 0) == 0.0f);
    CHECK_NEAR(get(1, 0, 0), 0.5, 0.0);
    CHECK(tile.halfs[1 * 4 + 1] == 0);  // +0
    CHECK_NEAR(get(2, 0, 3), 1.0, 0.0);
    CHECK(get(100, 0, 3) == 0.0f);
    CHECK(!canonicalize(tile, extent));  // already canonical
    CHECK(tileExtent(300, 200, 2, 1).validWidth == 44);
    CHECK(tileExtent(300, 200, 2, 1).validHeight == 72);
    CHECK(tileExtent(300, 200, 0, 0).validWidth == kTileSize);
}

// The saved-state marker follows undo and redo (spec §10.1 "*").
TEST_CASE(historyRevision) {
    using Ref = std::shared_ptr<int>;
    TileHistory<Ref> history(1 << 20, 1);
    const TileKey k{1, 0, 0};
    const auto clean = history.revision();
    history.commit({{k, nullptr, std::make_shared<int>(1)}});
    const auto first = history.revision();
    CHECK(first != clean);
    history.commit({{k, history.get(k), std::make_shared<int>(2)}});
    CHECK(history.revision() != first);
    history.undo();
    CHECK(history.revision() == first);
    history.undo();
    CHECK(history.revision() == clean);
    history.redo();
    CHECK(history.revision() == first);
    CHECK(!history.commit({{k, history.get(k), history.get(k)}}));  // no change, same revision
    CHECK(history.revision() == first);
    std::size_t visited = 0;
    history.forEachTile([&](const TileKey&, const Ref& r) { visited += r ? 1 : 0; });
    CHECK(visited == 1);
    history.replaceAll({{TileKey{1, 3, 3}, std::make_shared<int>(7)}});
    CHECK(history.size() == 0 && history.tileCount() == 1 && history.revision() != first);
}

// ── Layer tree (§7, §10.3) ───────────────────────────────────────────────────

namespace {

core::LayerProps propsNamed(const char* name) {
    core::LayerProps p;
    p.name = name;
    return p;
}

}  // namespace

TEST_CASE(layerTreeEdits) {
    core::LayerTree tree = core::LayerTree::single();
    CHECK(tree.size() == 1 && tree.active() == 1 && tree.nextId() == 2);
    CHECK(tree.find(1)->props.name == "Слой 1");

    // New nodes go above the active one and become active.
    const std::uint64_t l2 = tree.addRaster();
    CHECK(l2 == 2 && tree.active() == 2 && tree.find(2)->props.name == "Слой 2");
    const std::uint64_t g = tree.addGroup();
    CHECK(tree.children(0) == (std::vector<std::uint64_t>{1, 2, g}));
    CHECK(tree.find(g)->kind == core::NodeKind::Group && tree.find(g)->props.name == "Группа 1");

    // Into the group; a node never moves into its own subtree.
    CHECK(tree.move(1, g, 0));
    CHECK(tree.children(g) == (std::vector<std::uint64_t>{1}) && tree.find(1)->parent == g);
    CHECK(!tree.move(g, g, 0));
    const std::uint64_t inner = [&] {
        CHECK(tree.setActive(1));
        return tree.addGroup("Внутри");
    }();
    CHECK(tree.find(inner)->parent == g);
    CHECK(!tree.move(g, inner, 0));
    CHECK(tree.depth(inner) == 2 && tree.height(g) == 2);

    // Alt+↑ at the top of a group leaves it, right above the group.
    CHECK(tree.step(inner, 1) && tree.find(inner)->parent == 0);
    CHECK(tree.children(0) == (std::vector<std::uint64_t>{2, g, inner}));
    CHECK(!tree.step(inner, 1));  // already on top of the root
    CHECK(tree.step(inner, -1) && tree.children(0) == (std::vector<std::uint64_t>{2, inner, g}));

    // File order: parents first, siblings bottom to top.
    CHECK(tree.preorder() == (std::vector<std::uint64_t>{2, inner, g, 1}));
    CHECK(tree.rasters() == (std::vector<std::uint64_t>{2, 1}));
    CHECK(tree.rasters(g) == (std::vector<std::uint64_t>{1}));
}

TEST_CASE(layerTreeDuplicateAndRemove) {
    core::LayerTree tree = core::LayerTree::single();
    const std::uint64_t g = tree.addGroup();
    tree.setActive(1);
    tree.move(1, g, 0);
    const std::uint64_t top = [&] {
        tree.setActive(g);
        return tree.addRaster();
    }();
    // Duplicating a group copies its subtree with fresh IDs and reports the raster pairs.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> copies;
    const std::uint64_t copy = tree.duplicate(g, copies);
    CHECK(copy != 0 && tree.active() == copy);
    CHECK(tree.find(copy)->props.name == "Группа 1 копия");
    CHECK(copies.size() == 1 && copies[0].first == 1 && tree.find(copies[0].second)->parent == copy);
    CHECK(tree.children(0) == (std::vector<std::uint64_t>{g, copy, top}));

    // Removing the active node selects the neighbour below first.
    std::vector<std::uint64_t> removed;
    CHECK(tree.remove(copy, removed));
    CHECK(removed == (std::vector<std::uint64_t>{copies[0].second}));
    CHECK(tree.active() == g && !tree.find(copy));
    // ... then the one above when there is none below.
    tree.setActive(g);
    removed.clear();
    CHECK(tree.remove(g, removed) && removed == (std::vector<std::uint64_t>{1}));
    CHECK(tree.active() == top);
    // Deleting the last drawing layer creates an empty one in the same edit.
    removed.clear();
    CHECK(tree.remove(top, removed));
    CHECK(tree.size() == 1 && tree.rasters().size() == 1);
    const std::uint64_t fresh = tree.rasters().front();
    CHECK(fresh != top && tree.active() == fresh && tree.find(fresh)->props.name == "Слой 1");
}

TEST_CASE(layerTreeFlagsAndClips) {
    core::LayerTree tree = core::LayerTree::single();
    const std::uint64_t a = tree.addRaster();
    const std::uint64_t b = tree.addRaster();
    core::LayerProps clipped = tree.find(a)->props;
    clipped.clipToBase = true;
    tree.setProps(a, clipped);
    clipped = tree.find(b)->props;
    clipped.clipToBase = true;
    tree.setProps(b, clipped);
    CHECK(tree.clipBase(a) == 1 && tree.clipBase(b) == 1);  // consecutive clips share the base
    CHECK(tree.clipBase(1) == 0);
    tree.move(1, 0, 2);  // the base to the top: both become orphans
    CHECK(tree.clipBase(a) == 0 && tree.clipBase(b) == 0);

    // Visibility and locks are inherited from groups.
    const std::uint64_t g = tree.addGroup();
    tree.move(b, g, 0);
    core::LayerProps groupProps = tree.find(g)->props;
    groupProps.visible = false;
    groupProps.locked = true;
    tree.setProps(g, groupProps);
    CHECK(!tree.effectivelyVisible(b) && tree.effectivelyLocked(b));
    CHECK(tree.effectivelyVisible(a) && !tree.effectivelyLocked(a));
    // Properties are clamped; an unchanged value is not an edit.
    core::LayerProps wild = tree.find(a)->props;
    wild.opacity = 7.0;
    wild.blendId = 99;
    CHECK(tree.setProps(a, wild));
    CHECK(tree.find(a)->props.opacity == 1.0 && tree.find(a)->props.blendId == 26);
    CHECK(!tree.setProps(a, tree.find(a)->props));
    CHECK(core::blendModeName(0) == "Normal" && core::blendModeName(26) == "Invert" && core::blendModeName(27).empty());
}

TEST_CASE(layerTreeFromEntries) {
    std::string error;
    using E = core::LayerTree::Entry;
    const std::vector<E> good = {{5, 0, core::NodeKind::Group, propsNamed("G")},
                                 {3, 5, core::NodeKind::Raster, propsNamed("a")},
                                 {4, 5, core::NodeKind::Raster, propsNamed("b")},
                                 {7, 0, core::NodeKind::Raster, propsNamed("c")}};
    const auto tree = core::LayerTree::fromEntries(good, 4, 8, error);
    CHECK(tree.has_value());
    if (tree) {
        CHECK(tree->children(0) == (std::vector<std::uint64_t>{5, 7}));
        CHECK(tree->children(5) == (std::vector<std::uint64_t>{3, 4}));
        CHECK(tree->active() == 4 && tree->nextId() == 8);
    }
    CHECK(!core::LayerTree::fromEntries(good, 9, 8, error));  // unknown active
    CHECK(!core::LayerTree::fromEntries(good, 4, 7, error));  // nextId not above every ID
    std::vector<E> underRaster = good;
    underRaster[1].parent = 7;
    CHECK(!core::LayerTree::fromEntries(underRaster, 4, 8, error));
    const std::vector<E> cycle = {{1, 2, core::NodeKind::Group, propsNamed("x")}, {2, 1, core::NodeKind::Group, propsNamed("y")}};
    CHECK(!core::LayerTree::fromEntries(cycle, 1, 3, error));
    // Depth 64 is the limit.
    std::vector<E> deep;
    for (std::uint64_t i = 1; i <= 65; ++i) deep.push_back({i, i - 1, core::NodeKind::Group, propsNamed("g")});
    CHECK(!core::LayerTree::fromEntries(deep, 1, 66, error));
    deep.pop_back();
    CHECK(core::LayerTree::fromEntries(deep, 1, 66, error).has_value());
}

// ── Compositor (§7, §12.1) ───────────────────────────────────────────────────

namespace {

bool nearPixel(const core::Pixel& p, float r, float g, float b, float a, float tolerance = 1e-6f) {
    return std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance && std::abs(p.b - b) <= tolerance &&
           std::abs(p.a - a) <= tolerance;
}

core::Pixel opaque(float r, float g, float b) { return {r, g, b, 1.0f}; }

}  // namespace

TEST_CASE(compositorGoldenValues) {
    // §12.1: red S = (.5, 0, 0, .5), blue D = (0, 0, .5, .5), opacity .5.
    const core::Pixel s{0.5f, 0.0f, 0.0f, 0.5f};
    const core::Pixel d{0.0f, 0.0f, 0.5f, 0.5f};
    CHECK(nearPixel(core::blendPixel(d, s, 0.5f, 0), 0.25f, 0.0f, 0.375f, 0.625f));
    CHECK(nearPixel(core::blendPixel(d, s, 0.5f, 1), 0.125f, 0.0f, 0.375f, 0.625f));
    // Over transparent every mode gives (.25, 0, 0, .25); opacity 0 returns D bit for bit.
    for (int mode = 0; mode < core::kBlendModeCount; ++mode) {
        CHECK(nearPixel(core::blendPixel({}, s, 0.5f, mode), 0.25f, 0.0f, 0.0f, 0.25f));
        CHECK(core::blendPixel(d, s, 0.0f, mode) == d);
    }
    // Boundaries of the non-linear modes: Dodge (0, 1) → 0, Burn (1, 0) → 1, Divide (0, 0) → 0.
    CHECK(nearPixel(core::blendPixel(opaque(0, 0, 0), opaque(1, 1, 1), 1.0f, 5), 0, 0, 0, 1));
    CHECK(nearPixel(core::blendPixel(opaque(1, 1, 1), opaque(0, 0, 0), 1.0f, 9), 1, 1, 1, 1));
    CHECK(nearPixel(core::blendPixel(opaque(0, 0, 0), opaque(0, 0, 0), 1.0f, 21), 0, 0, 0, 1));
    // Hue of two greys keeps the channels equal.
    const core::Pixel grey = core::blendPixel(opaque(0.3f, 0.3f, 0.3f), opaque(0.7f, 0.7f, 0.7f), 1.0f, 22);
    CHECK(grey.r == grey.g && grey.g == grey.b);
    // Every mode stays finite and inside the coverage on random inputs.
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    const auto random = [&] {
        const float a = next();
        return core::Pixel{a * next(), a * next(), a * next(), a};
    };
    bool bounded = true;
    for (int i = 0; i < 2000; ++i) {
        const core::Pixel bottom = random();
        const core::Pixel top = random();
        for (int mode = 0; mode < core::kBlendModeCount; ++mode) {
            const core::Pixel o = core::blendPixel(bottom, top, next(), mode);
            for (const float c : {o.r, o.g, o.b, o.a}) bounded = bounded && std::isfinite(c) && c >= 0.0f;
            bounded = bounded && o.r <= o.a && o.g <= o.a && o.b <= o.a && o.a <= 1.0f;
        }
    }
    CHECK(bounded);
}

TEST_CASE(compositorTreeRules) {
    // Layers: 1 base (a = .25), 2 and 3 clipped opaque on top, 4 hidden; a group 5 with 6 inside.
    core::LayerTree tree = core::LayerTree::single();
    for (int i = 0; i < 3; ++i) tree.addRaster();
    core::LayerProps p = tree.find(2)->props;
    p.clipToBase = true;
    tree.setProps(2, p);
    p = tree.find(3)->props;
    p.clipToBase = true;
    tree.setProps(3, p);
    std::map<std::uint64_t, core::Pixel> colour = {
        {1, {0.25f * 0.2f, 0.25f * 0.4f, 0.25f * 0.6f, 0.25f}}, {2, opaque(1, 0, 0)}, {3, opaque(0, 1, 0)}, {4, opaque(0, 0, 1)}};
    const core::LayerFetch fetch = [&](std::uint64_t layer, std::span<core::Pixel> out) {
        const auto it = colour.find(layer);
        if (it == colour.end()) return false;
        std::fill(out.begin(), out.end(), it->second);
        return true;
    };
    const auto composite = [&] {
        core::Pixel out;
        core::compose(tree, std::span<core::Pixel>(&out, 1), fetch);
        return out;
    };
    // §12.1: the chain keeps the base alpha .25; the top clipped layer wins inside it.
    p = tree.find(4)->props;
    p.visible = false;
    tree.setProps(4, p);
    CHECK(nearPixel(composite(), 0.0f, 0.25f, 0.0f, 0.25f, 1e-3f));
    // A hidden base hides its chain.
    p = tree.find(1)->props;
    p.visible = false;
    tree.setProps(1, p);
    CHECK(nearPixel(composite(), 0, 0, 0, 0));
    // With the base gone the clips are orphans and contribute nothing.
    std::vector<std::uint64_t> removed;
    tree.remove(1, removed);
    CHECK(nearPixel(composite(), 0, 0, 0, 0));
    // An isolated group: Multiply inside it multiplies only what is inside.
    core::LayerTree groups = core::LayerTree::single();  // 1: white below the group
    const std::uint64_t g = groups.addGroup();
    const std::uint64_t inside = groups.addRaster();
    groups.move(inside, g, 0);
    p = groups.find(inside)->props;
    p.blendId = 1;
    groups.setProps(inside, p);
    colour = {{1, opaque(1, 1, 1)}, {inside, opaque(0.5f, 0.5f, 0.5f)}};
    core::Pixel out;
    core::compose(groups, std::span<core::Pixel>(&out, 1), fetch);
    CHECK(nearPixel(out, 0.5f, 0.5f, 0.5f, 1.0f, 1e-3f));  // over nothing inside the group: the layer itself
    groups.move(inside, 0, 2);                           // out of the group: Multiply with white
    core::compose(groups, std::span<core::Pixel>(&out, 1), fetch);
    CHECK(nearPixel(out, 0.5f, 0.5f, 0.5f, 1.0f, 1e-3f));
    colour[1] = opaque(0.5f, 0.5f, 0.5f);  // Multiply 0.5 × 0.5
    core::compose(groups, std::span<core::Pixel>(&out, 1), fetch);
    CHECK(nearPixel(out, 0.25f, 0.25f, 0.25f, 1.0f, 1e-3f));
}

TEST_CASE(historyAmendMeta) {
    using Ref = std::shared_ptr<const int>;
    core::TileHistory<Ref, core::LayerTree> history(1 << 20, 4);
    history.replaceAll({}, core::LayerTree::single());
    core::LayerTree tree = history.meta();
    tree.addRaster();
    CHECK(history.commit({}, tree));
    const std::uint64_t revision = history.revision();
    // Choosing the active layer is not a command and keeps the revision (the document is not dirty).
    tree.setActive(1);
    history.amendMeta(tree);
    CHECK(history.meta().active() == 1 && history.revision() == revision && history.size() == 1);
    // Undo restores the tree as it was before the command, with its active layer.
    CHECK(history.undo() != nullptr);
    CHECK(history.meta().size() == 1 && history.meta().active() == 1);
    CHECK(history.redo() != nullptr);
    CHECK(history.meta().size() == 2 && history.meta().active() == 2);
}

// ── Selection and raster operations (§10.1) ──────────────────────────────────

namespace {

std::shared_ptr<core::TileData> solid(float r, float g, float b, float a) {
    auto t = std::make_shared<core::TileData>();
    for (std::size_t i = 0; i < core::kTileHalfs; i += 4) {
        t->halfs[i] = core::floatToHalf(r * a);
        t->halfs[i + 1] = core::floatToHalf(g * a);
        t->halfs[i + 2] = core::floatToHalf(b * a);
        t->halfs[i + 3] = core::floatToHalf(a);
    }
    return t;
}

core::Pixel pixelOf(const std::vector<core::TileEdit>& edits, int tx, int ty, int x, int y) {
    for (const auto& e : edits) {
        if (e.tx != tx || e.ty != ty) continue;
        if (!e.after) return {};
        const std::size_t i = (static_cast<std::size_t>(y) * core::kTileSize + static_cast<std::size_t>(x)) * 4;
        return {core::halfToFloat(e.after->halfs[i]), core::halfToFloat(e.after->halfs[i + 1]),
                core::halfToFloat(e.after->halfs[i + 2]), core::halfToFloat(e.after->halfs[i + 3])};
    }
    return {-1, -1, -1, -1};  // not edited
}

}  // namespace

TEST_CASE(lassoCoverageAndOperations) {
    // A square from (10, 10) to (20.5, 20): full pixels inside, half coverage on the right column.
    const core::SelectionMask square = core::rasterizeLasso({{10, 10}, {20.5, 10}, {20.5, 20}, {10, 20}}, 300, 200);
    CHECK(square.at(15, 15) == 255 && square.at(10, 10) == 255 && square.at(19, 19) == 255);
    CHECK(square.at(20, 15) == 128);  // 8 of 16 samples
    CHECK(square.at(9, 15) == 0 && square.at(15, 20) == 0);
    CHECK(square.bounds() == (core::PixelBox{10, 10, 21, 20}));
    // Even–odd: the inner square of a doubled outline is a hole.
    const core::SelectionMask ring = core::rasterizeLasso(
        {{0, 0}, {40, 0}, {40, 40}, {0, 40}, {0, 0}, {10, 10}, {30, 10}, {30, 30}, {10, 30}, {10, 10}}, 300, 200);
    CHECK(ring.at(5, 5) == 255 && ring.at(20, 20) == 0);
    // §10.1 operations: add = max, intersect = min, subtract = a·(1 − b).
    const core::SelectionMask other = core::rasterizeLasso({{15, 0}, {30, 0}, {30, 30}, {15, 30}}, 300, 200);
    const auto add = core::combine(square, other, core::SelectOp::Add);
    const auto inter = core::combine(square, other, core::SelectOp::Intersect);
    const auto sub = core::combine(square, other, core::SelectOp::Subtract);
    CHECK(add.at(12, 12) == 255 && add.at(25, 25) == 255);
    CHECK(inter.at(12, 12) == 0 && inter.at(17, 12) == 255 && inter.at(25, 25) == 0);
    CHECK(sub.at(12, 12) == 255 && sub.at(17, 12) == 0);
    CHECK(core::combine(std::nullopt, other, core::SelectOp::Subtract) == other);  // no selection: replace
    // All, invert and move stay inside the document; the partial edge tile is zero outside it.
    const auto all = core::selectAll(300, 200);
    CHECK(all.at(299, 199) == 255 && all.at(300, 10) == 0 && all.tiles().size() == 6);
    const auto none = core::invert(all, 300, 200);
    CHECK(none.empty());
    const auto moved = core::translate(square, 5, -3, 300, 200);
    CHECK(moved.at(15, 7) == 255 && moved.at(25, 12) == 128 && moved.at(12, 12) == 0);
}

TEST_CASE(floodFillAndWand) {
    // Two colours: red left of x = 50, blue right; the wand from the left stops at the edge.
    const auto at = [](int x, int) { return x < 50 ? core::Pixel{1, 0, 0, 1} : core::Pixel{0, 0, 1, 1}; };
    const core::SelectionMask left = core::floodMask(at, 200, 100, 10, 10, 0.1, true);
    CHECK(left.at(49, 99) == 255 && left.at(50, 0) == 0 && left.bounds() == (core::PixelBox{0, 0, 50, 100}));
    // Not connected: every matching pixel of the canvas.
    const auto stripes = [](int x, int) { return (x / 10) % 2 ? core::Pixel{1, 1, 1, 1} : core::Pixel{}; };
    const core::SelectionMask allWhite = core::floodMask(stripes, 100, 10, 15, 5, 0.0, false);
    const core::SelectionMask oneWhite = core::floodMask(stripes, 100, 10, 15, 5, 0.0, true);
    CHECK(allWhite.at(95, 0) == 255 && oneWhite.at(95, 0) == 0 && oneWhite.at(19, 9) == 255);

    // Fill: source-over through region ∩ selection; Alpha Lock keeps a0 and leaves empty pixels.
    const core::TileSource half = [](int tx, int ty) -> core::TileContent {
        return tx == 0 && ty == 0 ? core::TileContent(solid(1, 0, 0, 0.5f)) : nullptr;
    };
    const core::SelectionMask region = core::selectAll(200, 100);
    const core::SelectionMask selection = core::rasterizeLasso({{0, 0}, {64, 0}, {64, 100}, {0, 100}}, 200, 100);
    const auto filled = core::fillRegion(half, region, selection, {0, 0, 1, 1}, false, 200, 100);
    CHECK(pixelOf(filled, 0, 0, 10, 10).b == 1.0f && pixelOf(filled, 0, 0, 10, 10).a == 1.0f);
    CHECK(pixelOf(filled, 0, 0, 100, 10).r == 0.5f);  // outside the selection: unchanged
    const auto locked = core::fillRegion(half, region, std::nullopt, {0, 0, 1, 1}, true, 200, 100);
    CHECK(std::abs(pixelOf(locked, 0, 0, 10, 10).a - 0.5f) < 1e-3f && std::abs(pixelOf(locked, 0, 0, 10, 10).b - 0.5f) < 1e-3f);
    CHECK(pixelOf(locked, 1, 0, 10, 10).a == -1.0f);  // tile (1, 0) is empty: nothing to recolour
}

TEST_CASE(moveCutCopyPaste) {
    auto content = std::make_shared<core::TileData>();
    for (int x = 0; x < 128; ++x) {  // a gradient row 5, so moves are visible
        const std::size_t i = (5 * 128 + static_cast<std::size_t>(x)) * 4;
        content->halfs[i] = core::floatToHalf(x / 255.0f);
        content->halfs[i + 3] = core::floatToHalf(1.0f);
    }
    const core::TileSource layer = [&](int tx, int ty) -> core::TileContent { return tx == 0 && ty == 0 ? content : nullptr; };
    // §10.1: whole-pixel moves copy exactly, across tiles; a zero move changes nothing.
    const auto moved = core::moveLayer(layer, {{0, 0}}, std::nullopt, 100, 3, 300, 200);
    const core::Pixel carried = pixelOf(moved, 1, 0, 50, 8);  // x = 178 ← 78
    CHECK(carried.r == core::halfToFloat(core::floatToHalf(78 / 255.0f)) && carried.a == 1.0f);
    CHECK(pixelOf(moved, 0, 0, 10, 5).a == 0.0f);  // the old place is empty
    CHECK(core::moveLayer(layer, {{0, 0}}, std::nullopt, 0, 0, 300, 200).empty());
    // With a mask only the masked part moves; the rest stays.
    const core::SelectionMask left = core::rasterizeLasso({{0, 0}, {64, 0}, {64, 128}, {0, 128}}, 300, 200);
    const auto masked = core::moveLayer(layer, {{0, 0}}, left, 0, 10, 300, 200);
    CHECK(pixelOf(masked, 0, 0, 10, 15).a == 1.0f && pixelOf(masked, 0, 0, 10, 5).a == 0.0f);
    CHECK(pixelOf(masked, 0, 0, 100, 5).a == 1.0f);  // unselected part stays
    // Cut clears the selection; copy and paste round-trip through straight sRGB8.
    const auto cut = core::clearMasked(layer, left, 300, 200);
    CHECK(pixelOf(cut, 0, 0, 10, 5).a == 0.0f && pixelOf(cut, 0, 0, 100, 5).a == 1.0f);
    const core::Image8 copy = core::extract(layer, {{0, 0}}, left, 300, 200);
    CHECK(copy.x == 0 && copy.y == 0 && copy.width == 64 && copy.height == 128);
    CHECK(copy.rgba[(5 * 64 + 10) * 4 + 3] == 255 && copy.rgba[3] == 0);
    core::Image8 placed = copy;
    placed.x = 250;  // partly outside: clipped
    const auto pasted = core::placeImage(placed, 300, 200);
    CHECK(pixelOf(pasted, 2, 0, 250 + 10 - 256, 5).a == 1.0f);  // image x 10 → document x 260, tile 2
    const core::Image8 whole = core::extract(layer, {{0, 0}}, std::nullopt, 300, 200);
    CHECK(whole.width == 128 && whole.height == 1 && whole.y == 5);  // the content's bounds
}

TEST_CASE(fxParametersAndCurves) {
    // §9.3: the identity table, linear interpolation, the point rules.
    const core::ToneCurve identity;
    const auto table = core::curveTable(identity);
    CHECK(table[0] == 0.0f && std::abs(table[128 * 4] - 128 / 255.0f) < 1e-6f && table[255 * 4 + 2] == 1.0f && table[3] == 1.0f);
    CHECK(core::evalCurve({{0, 0}, {0.5, 1}, {1, 0}}, 0.25) == 0.5);
    CHECK(core::validCurve({{0, 0}, {1, 1}}) && !core::validCurve({{0, 0}}) && !core::validCurve({{0.1, 0}, {1, 1}}));
    CHECK(!core::validCurve({{0, 0}, {0.5, 0.2}, {0.5, 0.3}, {1, 1}}));  // x strictly increasing
    // Master first, then the channel: C_r(Master(x)).
    core::ToneCurve curve;
    curve.channels[0] = {{0, 1}, {1, 0}};
    curve.channels[1] = {{0, 0}, {1, 0.5}};
    const auto t = core::curveTable(curve);
    CHECK(std::abs(t[0] - 0.5f) < 1e-6f && std::abs(t[1] - 1.0f) < 1e-6f);
    // Ranges are enforced on the CPU; halos follow §9.2–§9.6.
    core::FxSettings wild = core::defaultFx(core::FxKind::Chromatic);
    wild.shiftX = 100;
    wild.shiftY = std::numeric_limits<double>::quiet_NaN();
    wild.amount = -1;
    const core::FxSettings clean = core::sanitize(wild);
    CHECK(clean.shiftX == 32 && clean.shiftY == 0 && clean.amount == 0);
    CHECK(core::fxHalo(clean) == 33);
    CHECK(core::fxHalo(core::defaultFx(core::FxKind::Gaussian)) == 6 && core::fxHalo(core::defaultFx(core::FxKind::ToneCurve)) == 0);
    core::FxSettings lines = core::defaultFx(core::FxKind::AnimeLines);
    lines.radius = 20;
    lines.blackPoint = 0.5;
    lines.whitePoint = 0.5;
    const core::FxSettings fixed = core::sanitize(lines);
    CHECK(fixed.radius == 8 && fixed.whitePoint >= fixed.blackPoint + 1.0 / 255.0 && core::fxHalo(fixed) == 9);
}

// ── Vector lineart (§8, §8.1, §12.1 "Вектор") ───────────────────────────────

namespace {

core::Knot knot(std::uint64_t id, core::V p, core::V in, core::V out, double width) {
    return core::Knot{id, p, in, out, width, std::max(0.25, width), 0.0, 1.0, core::Join::smooth};
}

core::VectorObject sampleObject() {
    core::VectorObject o;
    o.id = 100;
    o.profile = core::builtInBrushes().front();
    o.knots = {knot(1, {10, 10}, {0, 0}, {30, -20}, 4), knot(2, {100, 40}, {-30, -10}, {0, 0}, 12)};
    return o;
}

}  // namespace

TEST_CASE(vectorSplitKeepsShape) {
    const core::VectorObject o = sampleObject();
    const core::Knot& l = o.knots[0];
    const core::Knot& r = o.knots[1];
    const auto parts = core::splitSegment(l, r, 0.5, 7);
    CHECK(parts[1].id == 7 && parts[1].join == core::Join::smooth);
    CHECK_NEAR(parts[1].width, 8.0, 1e-12);
    // Both halves trace the original curve: Bézier within 1e-6 px (§12.1).
    double worst = 0.0;
    for (int i = 0; i <= 20; ++i) {
        const double u = i / 20.0;
        const core::V a = core::bezier(parts[0].pos, parts[0].pos + parts[0].out, parts[1].pos + parts[1].in, parts[1].pos, u);
        const core::V b = core::bezier(l.pos, l.pos + l.out, r.pos + r.in, r.pos, u * 0.5);
        const core::V c = core::bezier(parts[1].pos, parts[1].pos + parts[1].out, parts[2].pos + parts[2].in, parts[2].pos, u);
        const core::V d = core::bezier(l.pos, l.pos + l.out, r.pos + r.in, r.pos, 0.5 + u * 0.5);
        worst = std::max({worst, core::norm(a - b), core::norm(c - d)});
    }
    CHECK(worst <= 1e-6);
    CHECK_THROWS(core::splitSegment(l, r, 1.0, 7));
    CHECK_THROWS(core::splitSegment(l, r, 0.5, 1));
    // The object helper inserts the knot in place and bumps the version.
    const core::VectorObject inserted = core::insertKnot(o, 0, 0.5, 7);
    CHECK(inserted.knots.size() == 3 && inserted.knots[1].id == 7 && inserted.version == 2);
}

TEST_CASE(vectorHandlesAndRemoval) {
    core::Knot k = knot(1, {0, 0}, {-3, 0}, {6, 0}, 2);
    k.join = core::Join::symmetric;
    core::setHandle(k, true, {0, 5});
    CHECK(k.in == (core::V{0, -5}));
    k.join = core::Join::smooth;
    core::setHandle(k, true, {4, 0});  // the other keeps its length 5, opposite direction
    CHECK(core::norm(k.in - core::V{-5, 0}) < 1e-12);
    k.join = core::Join::corner;
    core::setHandle(k, false, {1, 1});
    CHECK(k.out == (core::V{4, 0}));
    core::VectorObject o = core::insertKnot(sampleObject(), 0, 0.5, 7);
    const auto removed = core::removeKnot(o, 1);
    CHECK(removed && removed->knots.size() == 2 && removed->knots[0].id == 1);
    core::VectorObject single = sampleObject();
    single.knots.resize(1);
    CHECK(!core::removeKnot(single, 0));  // the last point removes the object
    CHECK(core::validateObject(sampleObject()).empty());
    core::VectorObject bad = sampleObject();
    bad.knots[1].id = 1;
    CHECK(!core::validateObject(bad).empty());
    bad = sampleObject();
    bad.knots[0].width = 5000;
    CHECK(!core::validateObject(bad).empty());
}

TEST_CASE(vectorFitAndReplay) {
    // Stamps along a straight line with a width ramp: the fit keeps few knots and the replay keeps
    // the widths along the arc.
    std::vector<core::Dab> dabs;
    for (int i = 0; i <= 100; ++i) {
        core::Dab d;
        d.center = {10.0 + i, 50.0};
        d.radius = 1.0 + i * 0.05;  // linear: two knots suffice
        d.inkWeight = 1.0;
        d.arc = i;
        dabs.push_back(d);
    }
    std::uint64_t next = 10;
    const auto knots = core::fitKnots(dabs, [&next] { return next++; });
    CHECK(knots.size() == 2 && knots[0].id == 10 && knots[1].id == 11);
    CHECK_NEAR(knots[0].width, 2.0, 1e-12);
    CHECK_NEAR(knots[1].width, 12.0, 1e-12);
    // A bend and a width bump need more knots.
    std::vector<core::Dab> bent = dabs;
    for (std::size_t i = 50; i < bent.size(); ++i) bent[i].center.y += (static_cast<double>(i) - 50) * 0.8;
    bent[25].radius += 3;
    CHECK(core::fitKnots(bent, [&next] { return next++; }).size() > 3);
    // Replay: round end stamps and the width at the middle of the arc.
    core::VectorObject o;
    o.id = 1;
    o.profile = core::builtInBrushes().front();
    o.knots = knots;
    const auto replay = core::objectDabs(o);
    CHECK(replay.size() > 5);
    CHECK(core::norm(replay.front().center - core::V{10, 50}) < 1e-9 && core::norm(replay.back().center - core::V{110, 50}) < 1e-9);
    const core::Dab& mid = *std::min_element(replay.begin(), replay.end(), [](const core::Dab& a, const core::Dab& b) {
        return std::abs(a.center.x - 60) < std::abs(b.center.x - 60);
    });
    CHECK_NEAR(2 * mid.radius, 7.0, 0.3);
    // Picking: the nearest point and its width.
    double distance = 0.0;
    const auto hit = core::nearest(o, {60, 53}, &distance);
    CHECK(hit && std::abs(distance - 3.0) < 1e-6 && std::abs(hit->point.x - 60) < 1e-6);
}

TEST_CASE(vectorEditsUndo) {
    // §12.1: edits are commands; undo restores the knots and widths exactly.
    using Ref = std::shared_ptr<const int>;
    core::TileHistory<Ref, core::DocumentMeta> history(1 << 20, 4);
    core::DocumentMeta meta;
    meta.layers = core::LayerTree::single();
    const std::uint64_t layer = meta.layers.addVector();
    meta.vectors[layer] = {sampleObject()};
    history.replaceAll({}, meta);
    core::DocumentMeta edited = history.meta();
    edited.vectors[layer][0] = core::insertKnot(edited.vectors[layer][0], 0, 0.5, edited.layers.reserveId());
    edited.vectors[layer][0].knots[1].width = 20;
    CHECK(history.commit({}, edited));
    CHECK(history.meta().vectors.at(layer)[0].knots.size() == 3);
    CHECK(history.undo() != nullptr);
    CHECK(history.meta().vectors.at(layer)[0] == sampleObject());
    // Rasterising turns the layer into a raster one and drops its objects (one command).
    core::DocumentMeta raster = history.meta();
    CHECK(raster.layers.rasterize(layer) && raster.layers.find(layer)->kind == core::NodeKind::Raster);
    CHECK(!raster.layers.rasterize(1));  // not a vector layer
}

int main() { return test::runAll(); }

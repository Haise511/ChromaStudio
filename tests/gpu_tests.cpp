// ChromaStudio Engine · engine_tests · GPU tile pipeline (hidden window, real OpenGL 3.3 context).
// Exits with 77 (CTest SKIP) when no OpenGL 3.3 context can be created.
// Every expected value follows from the §4.1 program and the §4 accumulation rules. Committed tiles
// are checked in their canonical RAM copy (binary16) and against the GPU copy, which must agree.

#include "TestHarness.hpp"

#include "core/Compositor.hpp"
#include "core/Half.hpp"
#include "core/Selection.hpp"
#include "core/Log.hpp"
#include "core/MaskImage.hpp"
#include "core/StrokeBuilder.hpp"
#include "render_gl/GlContext.hpp"
#include "render_gl/TileRenderer.hpp"
#include "ui/BrushPreview.hpp"
#include "ui/EditorUi.hpp"
#include "ui/UiRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <map>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace {

using chroma::core::Dab;
using namespace chroma::render;

chroma::gl::Context* g_context = nullptr;

constexpr double kHalfTolerance = 2e-3;

using TileMap = std::map<std::pair<int, int>, TileRef>;

TileLookup lookupIn(const TileMap& tiles) {
    return [&tiles](int tx, int ty) -> TileRef {
        const auto it = tiles.find({tx, ty});
        return it == tiles.end() ? TileRef{} : it->second;
    };
}

void publish(TileMap& tiles, const std::vector<TileChange>& changes) {
    for (const TileChange& c : changes) {
        if (c.after) {
            tiles[{c.key.tx, c.key.ty}] = c.after;
        } else {
            tiles.erase({c.key.tx, c.key.ty});
        }
    }
}

const TileChange* changeAt(const std::vector<TileChange>& changes, int tx, int ty) {
    for (const TileChange& c : changes) {
        if (c.key.tx == tx && c.key.ty == ty) return &c;
    }
    return nullptr;
}

struct Pixel {
    float r, g, b, a;
};

// From the canonical RAM copy.
Pixel pixelAt(const TileVersion& tile, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4;
    const auto& h = tile.data().halfs;
    using chroma::core::halfToFloat;
    return {halfToFloat(h[i]), halfToFloat(h[i + 1]), halfToFloat(h[i + 2]), halfToFloat(h[i + 3])};
}

bool gpuMatchesRam(TileRenderer& renderer, const TileVersion& tile) {
    const std::vector<float> gpu = renderer.readResident(tile);
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        if (gpu[i] != chroma::core::halfToFloat(tile.data().halfs[i])) return false;
    }
    return true;
}

Dab dab(double x, double y, double radius, double inkWeight) {
    Dab d;
    d.center = {x, y};
    d.radius = radius;
    d.inkWeight = inkWeight;
    return d;
}

BrushSetup inkSetup(bool maxMode, std::array<float, 4> ink = {0, 0, 0, 1}) {
    BrushSetup setup;
    setup.maxMode = maxMode;
    setup.inkPma = ink;
    return setup;
}

std::vector<TileChange> strokeOf(TileRenderer& renderer, const BrushSetup& setup, const TileMap& base,
                                 const std::vector<Dab>& dabs) {
    renderer.beginStroke(setup, lookupIn(base));
    renderer.applyDabs(dabs);
    return renderer.commitStroke();
}

}  // namespace

// One hard stamp in max mode: full ink inside, half at ρ = 1, nothing outside; PMA colour.
TEST_CASE(hardStampMax) {
    TileRenderer renderer(*g_context, 300, 200);
    const TileMap empty;
    const auto changes = strokeOf(renderer, inkSetup(true, {1, 0, 0, 1}), empty, {dab(64.5, 64.5, 10, 1)});
    CHECK(changes.size() == 1);
    const TileChange* c = changeAt(changes, 0, 0);
    CHECK(c && !c->before && c->after);
    if (!c || !c->after) return;
    const TileVersion& tile = *c->after;
    const Pixel centre = pixelAt(tile, 64, 64);
    CHECK_NEAR(centre.a, 1.0, kHalfTolerance);
    CHECK_NEAR(centre.r, 1.0, kHalfTolerance);
    const Pixel edge = pixelAt(tile, 74, 64);  // centre 74.5: ρ = 1 → smoothstep midpoint
    CHECK_NEAR(edge.a, 0.5, kHalfTolerance);
    CHECK_NEAR(edge.r, 0.5, kHalfTolerance);  // premultiplied
    CHECK_NEAR(edge.g, 0.0, 0.0);
    CHECK_NEAR(pixelAt(tile, 90, 64).a, 0.0, 0.0);
    CHECK_NEAR(pixelAt(tile, 0, 0).a, 0.0, 0.0);
    CHECK(chroma::core::validateTile(tile.data(), {kTileSize, kTileSize}).empty());
    CHECK(gpuMatchesRam(renderer, tile));
}

// §4: in max mode a second pass of the same gesture does not raise opacity.
TEST_CASE(maxModeIsIdempotent) {
    TileRenderer renderer(*g_context, 256, 256);
    const auto changes = strokeOf(renderer, inkSetup(true), {}, {dab(64, 64, 20, 0.5), dab(64, 64, 20, 0.5)});
    CHECK(changes.size() == 1);
    if (changes.empty() || !changes[0].after) return;
    CHECK_NEAR(pixelAt(*changes[0].after, 64, 64).a, 0.5, kHalfTolerance);
}

// §4: build mode composites each stamp source-over: 0.5 then 0.5 gives 0.75.
TEST_CASE(buildModeAccumulates) {
    TileRenderer renderer(*g_context, 256, 256);
    const auto changes = strokeOf(renderer, inkSetup(false), {}, {dab(64, 64, 20, 0.5), dab(64, 64, 20, 0.5)});
    CHECK(changes.size() == 1);
    if (changes.empty() || !changes[0].after) return;
    CHECK_NEAR(pixelAt(*changes[0].after, 64, 64).a, 0.75, kHalfTolerance);
    CHECK(gpuMatchesRam(renderer, *changes[0].after));
}

// §4: the eraser gives P′ = (1 − w)·P including alpha, in both accumulation modes; `before` is untouched.
TEST_CASE(eraserScalesPremultiplied) {
    TileRenderer renderer(*g_context, 256, 256);
    TileMap tiles;
    publish(tiles, strokeOf(renderer, inkSetup(true, {0.5f, 0.25f, 0.0f, 1.0f}), tiles, {dab(64, 64, 100, 1)}));
    CHECK(tiles.count({0, 0}) == 1);
    const TileRef opaque = tiles[{0, 0}];
    const auto opaqueBytes = opaque->data();

    for (const bool maxMode : {true, false}) {
        BrushSetup erase = inkSetup(maxMode);
        erase.erase = true;
        const auto changes = strokeOf(renderer, erase, tiles, {dab(64, 64, 10, 0.5)});
        const TileChange* c = changeAt(changes, 0, 0);
        CHECK(c && c->before == opaque && c->after);
        if (!c || !c->after) continue;
        const Pixel centre = pixelAt(*c->after, 64, 64);
        CHECK_NEAR(centre.a, 0.5, kHalfTolerance);
        CHECK_NEAR(centre.r, 0.25, kHalfTolerance);
        CHECK_NEAR(centre.g, 0.125, kHalfTolerance);
        CHECK_NEAR(pixelAt(*c->after, 10, 10).a, 1.0, kHalfTolerance);
        CHECK(opaque->data() == opaqueBytes);  // the history version is never written
    }
}

// §10.2: a tile erased completely becomes "no tile"; an unchanged result collapses to `before`.
TEST_CASE(transparentAndEqualResults) {
    TileRenderer renderer(*g_context, 256, 256);
    TileMap tiles;
    publish(tiles, strokeOf(renderer, inkSetup(true), tiles, {dab(64, 64, 8, 1)}));
    CHECK(tiles.count({0, 0}) == 1);

    // Black ink over solid black ink changes nothing.
    const auto same = strokeOf(renderer, inkSetup(true), tiles, {dab(64, 64, 3, 1)});
    const TileChange* unchanged = changeAt(same, 0, 0);
    CHECK(unchanged && unchanged->after == unchanged->before);

    BrushSetup erase = inkSetup(true);
    erase.erase = true;
    const auto erased = strokeOf(renderer, erase, tiles, {dab(64, 64, 40, 1)});
    const TileChange* gone = changeAt(erased, 0, 0);
    CHECK(gone && gone->before && !gone->after);
}

// A stamp across x = 128 is continuous over the tile seam.
TEST_CASE(seamIsContinuous) {
    TileRenderer renderer(*g_context, 512, 256);
    const auto changes = strokeOf(renderer, inkSetup(true), {}, {dab(128, 64, 19.6, 1)});
    CHECK(changes.size() == 2);
    const TileChange* left = changeAt(changes, 0, 0);
    const TileChange* right = changeAt(changes, 1, 0);
    CHECK(left && right && left->after && right->after);
    if (!left || !right || !left->after || !right->after) return;
    const TileVersion& l = *left->after;
    const TileVersion& r = *right->after;
    CHECK_NEAR(pixelAt(l, 127, 64).a, pixelAt(r, 0, 64).a, 0.0);   // both 0.5 px from the centre
    CHECK_NEAR(pixelAt(l, 120, 64).a, pixelAt(r, 7, 64).a, 0.0);   // both 7.5 px
    CHECK_NEAR(pixelAt(l, 108, 64).a, pixelAt(r, 19, 64).a, 0.0);  // both 19.5 px: on the soft edge
    CHECK(pixelAt(l, 108, 64).a > 0.05f && pixelAt(l, 108, 64).a < 0.95f);
}

// §2: the invisible remainder of an edge tile stays zero.
TEST_CASE(documentEdgeStaysZero) {
    TileRenderer renderer(*g_context, 300, 200);
    const auto changes = strokeOf(renderer, inkSetup(true), {}, {dab(295, 64, 20, 1), dab(295, 195, 20, 1)});
    const TileChange* right = changeAt(changes, 2, 0);
    const TileChange* corner = changeAt(changes, 2, 1);
    CHECK(right && corner && right->after && corner->after);
    if (!right || !corner || !right->after || !corner->after) return;
    CHECK_NEAR(pixelAt(*right->after, 43, 64).a, 1.0, kHalfTolerance);  // document x = 299
    CHECK(chroma::core::validateTile(right->after->data(), chroma::core::tileExtent(300, 200, 2, 0)).empty());
    CHECK_NEAR(pixelAt(*corner->after, 40, 71).a, 1.0, kHalfTolerance);  // document (296, 199)
    CHECK(chroma::core::validateTile(corner->after->data(), chroma::core::tileExtent(300, 200, 2, 1)).empty());
}

// The provisional tail is shown but never committed; cancel leaves nothing behind.
TEST_CASE(tailAndCancel) {
    TileRenderer renderer(*g_context, 256, 256);
    renderer.beginStroke(inkSetup(true), {});
    renderer.applyDabs(std::vector<Dab>{dab(30, 30, 8, 1)});
    renderer.setTail(std::vector<Dab>{dab(200, 200, 8, 1)});
    CHECK(renderer.strokeTileCount() == 2);
    renderer.setTail(std::vector<Dab>{dab(90, 90, 8, 1)});
    const auto changes = renderer.commitStroke();
    CHECK(changes.size() == 1);  // tile (1, 1) was only ever touched by the tail
    const TileChange* c = changeAt(changes, 0, 0);
    CHECK(c && c->after);
    if (c && c->after) {
        CHECK_NEAR(pixelAt(*c->after, 30, 30).a, 1.0, kHalfTolerance);
        CHECK_NEAR(pixelAt(*c->after, 90, 90).a, 0.0, 0.0);
    }

    renderer.beginStroke(inkSetup(false), {});
    renderer.applyDabs(std::vector<Dab>{dab(30, 30, 8, 1)});
    renderer.cancelStroke();
    CHECK(!renderer.strokeActive());
    CHECK(renderer.commitStroke().empty());
}

// Replay restarts from the base: only the replayed stamps remain, untouched tiles drop out.
TEST_CASE(replayStartsFromBase) {
    TileRenderer renderer(*g_context, 512, 256);
    renderer.beginStroke(inkSetup(false), {});
    renderer.applyDabs(std::vector<Dab>{dab(30, 30, 8, 1), dab(300, 30, 8, 1)});
    renderer.replay(std::vector<Dab>{dab(60, 60, 8, 0.5)});
    const auto changes = renderer.commitStroke();
    CHECK(changes.size() == 1);
    const TileChange* c = changeAt(changes, 0, 0);
    CHECK(c && c->after);
    if (!c || !c->after) return;
    CHECK_NEAR(pixelAt(*c->after, 30, 30).a, 0.0, 0.0);
    CHECK_NEAR(pixelAt(*c->after, 60, 60).a, 0.5, kHalfTolerance);
}

// §4 screentone: density sets the share of ink.
TEST_CASE(screentoneDensity) {
    TileRenderer renderer(*g_context, 256, 256);
    BrushSetup setup = inkSetup(true);
    setup.kind = 3;
    setup.periodPx = 8;
    setup.density = 0.35f;
    setup.toneAngle = static_cast<float>(std::numbers::pi / 4);
    const auto changes = strokeOf(renderer, setup, {}, {dab(64, 64, 60, 1)});
    CHECK(changes.size() == 1);
    if (changes.empty() || !changes[0].after) return;
    double sum = 0.0;
    for (int y = 32; y < 96; ++y) {
        for (int x = 32; x < 96; ++x) sum += pixelAt(*changes[0].after, x, y).a;
    }
    CHECK_NEAR(sum / (64.0 * 64.0), 0.35, 0.03);
}

// The texture kind multiplies the cover by the seeded paper grain: deterministic, inside [0, 1].
TEST_CASE(textureGrainDeterministic) {
    TileRenderer renderer(*g_context, 256, 256);
    BrushSetup setup = inkSetup(false);
    setup.kind = 2;
    setup.grainPx = 2.5f;
    setup.grainAmount = 0.85f;
    setup.seed = 1847;
    const auto first = strokeOf(renderer, setup, {}, {dab(64, 64, 40, 1)});
    const auto second = strokeOf(renderer, setup, {}, {dab(64, 64, 40, 1)});
    CHECK(first.size() == 1 && second.size() == 1);
    if (first.empty() || second.empty() || !first[0].after || !second[0].after) return;
    CHECK(first[0].after->data() == second[0].after->data());
    float lo = 1.0f, hi = 0.0f;
    for (int x = 50; x < 78; ++x) {
        lo = std::min(lo, pixelAt(*first[0].after, x, 64).a);
        hi = std::max(hi, pixelAt(*first[0].after, x, 64).a);
    }
    CHECK(lo >= 0.0f && hi <= 1.0f && hi - lo > 0.1f);  // the grain is visible
}

// §14.2: a tip mask shapes the stamp (uv = local·½ + ½, LINEAR, CLAMP); the next stroke without one
// is the procedural tip again.
TEST_CASE(tipMaskShapesStamp) {
    TileRenderer renderer(*g_context, 256, 256);
    auto mask = std::make_shared<chroma::core::MaskImage>();
    mask->width = 4;
    mask->height = 4;
    mask->r8.assign(16, 0);
    for (int y = 0; y < 4; ++y) mask->r8[static_cast<std::size_t>(y) * 4] = mask->r8[static_cast<std::size_t>(y) * 4 + 1] = 255;
    BrushSetup setup = inkSetup(true);
    setup.tipMask = mask;
    const auto masked = strokeOf(renderer, setup, {}, {dab(64.5, 64.5, 20, 1)});
    CHECK(masked.size() == 1 && masked[0].after);
    if (masked.empty() || !masked[0].after) return;
    const TileVersion& tile = *masked[0].after;
    CHECK_NEAR(pixelAt(tile, 54, 64).a, 1.0, kHalfTolerance);  // the left half of the mask
    CHECK_NEAR(pixelAt(tile, 54, 58).a, 1.0, kHalfTolerance);
    CHECK_NEAR(pixelAt(tile, 74, 64).a, 0.0, 0.0);             // the right half: nothing
    CHECK_NEAR(pixelAt(tile, 64, 64).a, 0.5, 0.02);            // between the texels: linear
    const auto plain = strokeOf(renderer, inkSetup(true), {}, {dab(64.5, 64.5, 20, 1)});
    CHECK(plain.size() == 1 && plain[0].after && pixelAt(*plain[0].after, 74, 64).a > 0.99f);
}

// §12.1 "потеря контекста": versions outlive their renderer and context; a new context uploads
// them again from RAM with identical content.
TEST_CASE(deviceLossReuploadsFromRam) {
    TileRef version;
    {
        TileRenderer first(*g_context, 256, 256);
        const auto changes = strokeOf(first, inkSetup(true, {0.2f, 0.4f, 0.6f, 1.0f}), {}, {dab(70, 50, 30, 0.8)});
        CHECK(changes.size() == 1);
        if (changes.empty()) return;
        version = changes[0].after;
    }
    CHECK(version != nullptr);
    if (!version) return;

    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ChromaStudio.GpuTests.Second";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    std::string failure;
    {
        std::unique_ptr<chroma::gl::Context> second = chroma::gl::Context::create(window, failure);
        CHECK(second != nullptr);
        if (second) {
            TileRenderer renderer(*second, 256, 256);
            CHECK(gpuMatchesRam(renderer, *version));
        }
    }
    DestroyWindow(window);
    g_context->makeCurrent();
}

// ── Layers and the compositor (§7, §12.2: GPU/CPU parity within 0.002 per operation) ──

namespace {

using LayerTiles = std::map<std::uint64_t, TileRef>;  // one tile (0, 0) per layer

/// A canonical PMA tile of pseudo-random content; `coverage` < 1 leaves some pixels transparent.
TileRef randomTile(std::uint32_t seed, float coverage = 1.0f) {
    auto data = std::make_shared<chroma::core::TileData>();
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTileSize) * kTileSize; ++i) {
        const float a = next() < coverage ? next() : 0.0f;
        const float rgb[3] = {a * next(), a * next(), a * next()};
        for (int c = 0; c < 3; ++c) data->halfs[4 * i + static_cast<std::size_t>(c)] = chroma::core::floatToHalf(rgb[c]);
        data->halfs[4 * i + 3] = chroma::core::floatToHalf(a);
    }
    chroma::core::canonicalize(*data, {kTileSize, kTileSize});
    return TileRenderer::makeVersion(std::move(data));
}

TileRef solidTile(float r, float g, float b, float a, int x0 = 0, int x1 = kTileSize) {
    auto data = std::make_shared<chroma::core::TileData>();
    for (int y = 0; y < kTileSize; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4;
            data->halfs[i] = chroma::core::floatToHalf(r * a);
            data->halfs[i + 1] = chroma::core::floatToHalf(g * a);
            data->halfs[i + 2] = chroma::core::floatToHalf(b * a);
            data->halfs[i + 3] = chroma::core::floatToHalf(a);
        }
    }
    return TileRenderer::makeVersion(std::move(data));
}

LayerStack stackOf(const chroma::core::LayerTree& tree, const LayerTiles& tiles) {
    LayerStack stack;
    stack.tree = &tree;
    stack.tiles = [&tiles](std::uint64_t layer, int tx, int ty) -> TileRef {
        if (tx != 0 || ty != 0) return nullptr;
        const auto it = tiles.find(layer);
        return it == tiles.end() ? nullptr : it->second;
    };
    return stack;
}

/// The CPU reference of tile (0, 0).
std::vector<chroma::core::Pixel> cpuComposite(const chroma::core::LayerTree& tree, const LayerTiles& tiles) {
    std::vector<chroma::core::Pixel> out(static_cast<std::size_t>(kTileSize) * kTileSize);
    chroma::core::compose(tree, out, [&](std::uint64_t layer, std::span<chroma::core::Pixel> px) {
        const auto it = tiles.find(layer);
        if (it == tiles.end() || !it->second) return false;
        const auto& h = it->second->data().halfs;
        using chroma::core::halfToFloat;
        for (std::size_t i = 0; i < px.size(); ++i) {
            px[i] = {halfToFloat(h[4 * i]), halfToFloat(h[4 * i + 1]), halfToFloat(h[4 * i + 2]), halfToFloat(h[4 * i + 3])};
        }
        return true;
    });
    return out;
}

double maxDifference(const std::vector<float>& gpu, const std::vector<chroma::core::Pixel>& cpu) {
    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const float c[4] = {cpu[i].r, cpu[i].g, cpu[i].b, cpu[i].a};
        for (std::size_t k = 0; k < 4; ++k) worst = std::max(worst, static_cast<double>(std::abs(gpu[4 * i + k] - c[k])));
    }
    return worst;
}

void setProps(chroma::core::LayerTree& tree, std::uint64_t id, const std::function<void(chroma::core::LayerProps&)>& edit) {
    chroma::core::LayerProps p = tree.find(id)->props;
    edit(p);
    tree.setProps(id, p);
}

}  // namespace

// Every blend mode: one operation on random PMA data, GPU against the CPU reference.
TEST_CASE(compositorModesMatchCpu) {
    TileRenderer renderer(*g_context, 128, 128);
    chroma::core::LayerTree tree = chroma::core::LayerTree::single();
    const std::uint64_t top = tree.addRaster();
    const LayerTiles tiles = {{1, randomTile(7, 0.9f)}, {top, randomTile(99, 0.9f)}};
    const LayerStack stack = stackOf(tree, tiles);
    double worst = 0.0;
    int worstMode = -1;
    for (int mode = 0; mode < chroma::core::kBlendModeCount; ++mode) {
        setProps(tree, top, [&](chroma::core::LayerProps& p) {
            p.blendId = mode;
            p.opacity = 0.7;
        });
        const double d = maxDifference(renderer.readComposite(stack, 0, 0), cpuComposite(tree, tiles));
        if (d > worst) {
            worst = d;
            worstMode = mode;
        }
    }
    if (worst > 0.002) std::printf("    worst difference %.5f in mode %d\n", worst, worstMode);
    CHECK(worst <= 0.002);
}

// Groups, a clip chain, a hidden layer and an orphan clip: the tree rules on both paths.
TEST_CASE(compositorTreeMatchesCpu) {
    TileRenderer renderer(*g_context, 128, 128);
    using chroma::core::LayerProps;
    chroma::core::LayerTree tree = chroma::core::LayerTree::single();  // 1
    const std::uint64_t orphan = [&] {
        tree.setActive(1);
        const std::uint64_t id = tree.addRaster();
        tree.move(id, 0, 0);  // the very bottom: nothing to clip to
        return id;
    }();
    setProps(tree, orphan, [](LayerProps& p) { p.clipToBase = true; });
    tree.setActive(1);
    const std::uint64_t clipA = tree.addRaster();
    const std::uint64_t clipB = tree.addRaster();
    setProps(tree, clipA, [](LayerProps& p) {
        p.clipToBase = true;
        p.blendId = 3;
        p.opacity = 0.6;
    });
    setProps(tree, clipB, [](LayerProps& p) { p.clipToBase = true; });
    setProps(tree, 1, [](LayerProps& p) {
        p.blendId = 2;
        p.opacity = 0.8;
    });
    const std::uint64_t group = tree.addGroup();
    setProps(tree, group, [](LayerProps& p) {
        p.blendId = 1;
        p.opacity = 0.9;
    });
    const std::uint64_t inA = tree.addRaster();
    const std::uint64_t inB = tree.addRaster();
    tree.move(inA, group, 0);
    tree.move(inB, group, 1);
    setProps(tree, inB, [](LayerProps& p) { p.blendId = 18; });
    const std::uint64_t hidden = tree.addRaster();
    setProps(tree, hidden, [](LayerProps& p) { p.visible = false; });
    const LayerTiles tiles = {{1, randomTile(1, 0.8f)},      {orphan, randomTile(2)}, {clipA, randomTile(3)},
                              {clipB, randomTile(4, 0.5f)},  {inA, randomTile(5)},    {inB, randomTile(6, 0.7f)},
                              {hidden, randomTile(8)}};
    const LayerStack stack = stackOf(tree, tiles);
    const double d = maxDifference(renderer.readComposite(stack, 0, 0), cpuComposite(tree, tiles));
    if (d > 0.004) std::printf("    difference %.5f\n", d);
    CHECK(d <= 0.004);  // up to three operations deep
}

// §7 Alpha Lock: paint changes the colour inside the existing alpha only; the eraser does nothing.
TEST_CASE(alphaLockKeepsAlpha) {
    for (const bool maxMode : {true, false}) {
        TileRenderer renderer(*g_context, 128, 128);
        TileMap base;
        base[{0, 0}] = solidTile(1, 0, 0, 0.5f, 0, 64);  // left half: red at alpha .5; right half empty
        BrushSetup setup = inkSetup(maxMode, {0, 0, 1, 1});
        setup.alphaLock = true;
        const auto changes = strokeOf(renderer, setup, base, {dab(64, 64, 40, 1), dab(70, 64, 40, 1)});
        CHECK(changes.size() == 1);
        if (changes.empty() || !changes[0].after) continue;
        const Pixel inside = pixelAt(*changes[0].after, 60, 64);
        CHECK_NEAR(inside.a, 0.5, kHalfTolerance);  // a0 kept
        CHECK_NEAR(inside.b, 0.5, kHalfTolerance);  // blue at full weight, premultiplied by a0
        CHECK_NEAR(inside.r, 0.0, kHalfTolerance);
        const Pixel outside = pixelAt(*changes[0].after, 80, 64);
        CHECK(outside.a == 0.0f && outside.b == 0.0f);  // empty stays empty
        // The eraser under the lock leaves the tile as it was.
        BrushSetup eraser = setup;
        eraser.erase = true;
        const auto erased = strokeOf(renderer, eraser, base, {dab(40, 64, 30, 1)});
        CHECK(std::all_of(erased.begin(), erased.end(), [](const TileChange& c) { return c.before == c.after; }));
    }
}

// The display cache follows the tree's render state and the tile versions it was composed from. Drawn
// into the back buffer of a hidden 64 × 64 popup window with its own context.
TEST_CASE(displayCacheFollowsChanges) {
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ChromaStudio.GpuTests.Cache";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    std::string failure;
    {
        std::unique_ptr<chroma::gl::Context> context = window ? chroma::gl::Context::create(window, failure) : nullptr;
        CHECK(context != nullptr);
        if (context) {
            TileRenderer renderer(*context, 128, 128);
            chroma::core::LayerTree tree = chroma::core::LayerTree::single();
            const std::uint64_t top = tree.addRaster();
            setProps(tree, top, [](chroma::core::LayerProps& p) { p.opacity = 0.5; });
            LayerTiles tiles = {{1, solidTile(1, 0, 0, 1)}, {top, solidTile(0, 0, 1, 1)}};
            const LayerStack stack = stackOf(tree, tiles);
            DocumentFrame frame;
            frame.clientWidth = frame.clientHeight = 64;
            frame.viewportX1 = frame.viewportY1 = 64;
            frame.pixelsPerDocPx = 0.5;  // the whole tile in the 64 × 64 back buffer
            const auto centre = [&] {
                renderer.drawDocument(frame, stack);
                glFinish();
                std::array<std::uint8_t, 4> px{};
                glReadBuffer(GL_BACK);
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                return px;
            };
            const auto purple = centre();  // linear (.5, 0, .5) encoded once: 188
            if (std::abs(purple[0] - 188) > 2) std::printf("    centre %d %d %d\n", purple[0], purple[1], purple[2]);
            CHECK(std::abs(purple[0] - 188) <= 2 && purple[1] <= 2 && std::abs(purple[2] - 188) <= 2);
            CHECK(renderer.cachedTiles() == 1);
            CHECK(centre() == purple);  // from the cache
            setProps(tree, top, [](chroma::core::LayerProps& p) { p.opacity = 0.0; });
            const auto red = centre();
            CHECK(red[0] >= 253 && red[2] <= 2);
            tiles[1] = solidTile(0, 1, 0, 1);  // a new version of the bottom layer
            const auto green = centre();
            CHECK(green[1] >= 253 && green[0] <= 2);
            // One visible layer at full opacity is drawn as it is, without a composed copy.
            setProps(tree, top, [](chroma::core::LayerProps& p) { p.visible = false; });
            centre();
            CHECK(renderer.cachedTiles() == 0);
        }
    }
    if (window) DestroyWindow(window);
    g_context->makeCurrent();
}

// ── Wet stamps (§5, §12.1 "Пигмент") ─────────────────────────────────────────

namespace {

BrushSetup wetSetup(StrokeKind kind, std::array<float, 4> ink = {0, 0, 0, 1}) {
    BrushSetup s = inkSetup(false, ink);
    s.stroke = kind;
    s.blending = 1.0f;
    s.dilution = 0.0f;
    s.bleed = 0.7f;
    s.strength = 1.0f;
    return s;
}

std::vector<Dab> dabsAlong(double x0, double x1, double y, double radius, double step) {
    std::vector<Dab> out;
    for (double x = x0; x <= x1 + 1e-9; x += step) out.push_back(dab(x, y, radius, 1.0));
    return out;
}

/// Tile (tx, 0) of an opaque field given by colourAt(document x).
TileRef fieldTile(int tx, const std::function<std::array<float, 3>(int x)>& colourAt) {
    auto data = std::make_shared<chroma::core::TileData>();
    for (int y = 0; y < kTileSize; ++y) {
        for (int x = 0; x < kTileSize; ++x) {
            const auto c = colourAt(tx * kTileSize + x);
            const std::size_t i = (static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4;
            for (std::size_t k = 0; k < 3; ++k) data->halfs[i + k] = chroma::core::floatToHalf(c[k]);
            data->halfs[i + 3] = chroma::core::floatToHalf(1.0f);
        }
    }
    return TileRenderer::makeVersion(std::move(data));
}

/// The committed state after a stroke: base with the changes applied.
TileMap applied(const TileMap& base, const std::vector<TileChange>& changes) {
    TileMap out = base;
    publish(out, changes);
    return out;
}

Pixel docPixel(const TileMap& tiles, int x, int y) {
    const auto it = tiles.find({x / kTileSize, y / kTileSize});
    if (it == tiles.end() || !it->second) return {0, 0, 0, 0};
    return pixelAt(*it->second, x % kTileSize, y % kTileSize);
}

double pixelDistance(const Pixel& a, const Pixel& b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}

}  // namespace

// §12.1: a finger (b = 1, d = 0, e = 1) gliding over an even opaque field keeps the field, across a seam.
TEST_CASE(smudgeKeepsEvenField) {
    TileRenderer renderer(*g_context, 256, 128);
    const auto colour = [](int) { return std::array<float, 3>{0.3f, 0.6f, 0.2f}; };
    TileMap base;
    base[{0, 0}] = fieldTile(0, colour);
    base[{1, 0}] = fieldTile(1, colour);
    BrushSetup setup = wetSetup(StrokeKind::Smudge);
    setup.bleed = 1.0f;
    const auto changes = strokeOf(renderer, setup, base, dabsAlong(40, 220, 64, 20, 5));
    CHECK(!changes.empty());
    const TileMap after = applied(base, changes);
    double worst = 0.0;
    for (int y = 30; y < 100; y += 3) {
        for (int x = 0; x < 256; x += 3) worst = std::max(worst, pixelDistance(docPixel(after, x, y), docPixel(base, x, y)));
    }
    if (worst > kHalfTolerance) std::printf("    worst %.5f\n", worst);
    CHECK(worst <= kHalfTolerance);
}

// §12.1: red carried to the right across a red/white edge; the same stroke across the tile seam at
// x = 128 and away from it gives the same pixels.
TEST_CASE(smudgeCarriesAcrossSeam) {
    const auto stroke = [](int edge, double from) {
        TileRenderer renderer(*g_context, 256, 128);
        const auto colour = [edge](int x) {
            return x < edge ? std::array<float, 3>{1.0f, 0.0f, 0.0f} : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        };
        TileMap base;
        base[{0, 0}] = fieldTile(0, colour);
        base[{1, 0}] = fieldTile(1, colour);
        return applied(base, strokeOf(renderer, wetSetup(StrokeKind::Smudge), base, dabsAlong(from, from + 70, 64, 12, 3)));
    };
    const TileMap seam = stroke(100, 80);   // crosses x = 128
    const TileMap inside = stroke(36, 16);  // the same, 64 px to the left, inside tile 0
    const Pixel carried = docPixel(seam, 112, 64);
    CHECK(carried.g < 0.9f && carried.r > 0.95f);       // red moved into the white
    CHECK(docPixel(seam, 112, 110).g == 1.0f);          // outside the stroke: untouched
    double worst = 0.0;
    for (int y = 48; y <= 80; ++y) {
        for (int x = 70; x <= 170; ++x) worst = std::max(worst, pixelDistance(docPixel(seam, x, y), docPixel(inside, x - 64, y)));
    }
    if (worst > 0.002) std::printf("    seam difference %.5f\n", worst);
    CHECK(worst <= 0.002);
}

// §5: the finger's first stamp only picks up paint.
TEST_CASE(smudgeFirstStampPicksUp) {
    TileRenderer renderer(*g_context, 128, 128);
    TileMap base;
    base[{0, 0}] = solidTile(1, 0, 0, 1, 0, 64);
    CHECK(strokeOf(renderer, wetSetup(StrokeKind::Smudge), base, {dab(64, 64, 20, 1)}).empty());
}

// Watercolor lays fresh paint on an empty layer; blur softens an edge and keeps an even field.
TEST_CASE(watercolorAndBlur) {
    {
        TileRenderer renderer(*g_context, 128, 128);
        BrushSetup setup = wetSetup(StrokeKind::Watercolor, {0.0f, 0.0f, 0.8f, 1.0f});
        setup.blending = 0.72f;
        setup.dilution = 0.22f;
        setup.bleed = 0.65f;
        const auto changes = strokeOf(renderer, setup, {}, dabsAlong(30, 100, 64, 16, 4));
        CHECK(changes.size() == 1);
        if (!changes.empty() && changes[0].after) {
            const Pixel p = pixelAt(*changes[0].after, 64, 64);
            CHECK(p.a > 0.05f && p.b > p.r && p.b <= p.a + 1e-3f);
            CHECK(pixelAt(*changes[0].after, 64, 120).a == 0.0f);
        }
    }
    {
        TileRenderer renderer(*g_context, 128, 128);
        TileMap base;
        base[{0, 0}] = fieldTile(0, [](int x) {
            return x < 64 ? std::array<float, 3>{0.0f, 0.0f, 0.0f} : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        });
        BrushSetup setup = wetSetup(StrokeKind::Blur);
        setup.blurRadius = 6;
        const TileMap after = applied(base, strokeOf(renderer, setup, base, {dab(64, 40, 14, 1), dab(64, 48, 14, 1)}));
        const Pixel edge = docPixel(after, 64, 44);
        CHECK(edge.r > 0.1f && edge.r < 0.9f && std::abs(edge.a - 1.0f) < kHalfTolerance);
        CHECK(docPixel(after, 64, 110).r == 1.0f);  // outside the brush
        CHECK(docPixel(after, 30, 44).r == 0.0f);   // far inside the black: still black
    }
}

// §7: under Alpha Lock the wet tools keep the alpha of every pixel.
TEST_CASE(wetAlphaLock) {
    TileRenderer renderer(*g_context, 128, 128);
    TileMap base;
    base[{0, 0}] = solidTile(1, 0, 0, 0.5f, 0, 64);  // red at alpha .5 on the left, empty on the right
    BrushSetup setup = wetSetup(StrokeKind::Smudge);
    setup.alphaLock = true;
    const TileMap after = applied(base, strokeOf(renderer, setup, base, dabsAlong(30, 110, 64, 16, 4)));
    bool kept = true;
    for (int x = 0; x < 128; x += 2) kept = kept && std::abs(docPixel(after, x, 64).a - docPixel(base, x, 64).a) <= kHalfTolerance;
    CHECK(kept);
}

// §10.1: the selection limits every write, dry and wet; an empty selection forbids writing everywhere.
TEST_CASE(selectionLimitsStrokes) {
    const chroma::core::SelectionMask left =
        chroma::core::rasterizeLasso({{0, 0}, {64, 0}, {64, 128}, {0, 128}}, 256, 128);
    const auto strokeWith = [&](const BrushSetup& setup, const chroma::core::SelectionMask* mask) {
        TileRenderer renderer(*g_context, 256, 128);
        renderer.setSelection(mask);
        return applied({}, strokeOf(renderer, setup, {}, dabsAlong(20, 200, 64, 10, 4)));
    };
    BrushSetup watercolor = wetSetup(StrokeKind::Watercolor, {0, 0, 1, 1});
    watercolor.blending = 0.5f;  // half fresh paint: something to lay on an empty layer
    for (const BrushSetup& setup : {inkSetup(true), inkSetup(false), watercolor}) {
        const TileMap inside = strokeWith(setup, &left);
        CHECK(docPixel(inside, 30, 64).a > 0.2f);
        CHECK(docPixel(inside, 100, 64).a == 0.0f && docPixel(inside, 180, 64).a == 0.0f);
        const TileMap free = strokeWith(setup, nullptr);
        CHECK(docPixel(free, 180, 64).a > 0.2f);
    }
    const chroma::core::SelectionMask empty;
    CHECK(strokeWith(inkSetup(true), &empty).empty());
}

// ── Filters (§9, §12.1 "FX" and "Извлечение линий") ─────────────────────────

namespace {

/// Runs a filter on every tile of a w × h document whose tiles are `base`; returns the result.
TileMap runFx(const chroma::core::FxSettings& fx, const TileMap& base, int w, int h,
              const chroma::core::SelectionMask* selection = nullptr, bool alphaLock = false) {
    TileRenderer renderer(*g_context, w, h);
    renderer.setSelection(selection);
    FxRequest request;
    request.settings = fx;
    request.source = request.target = 1;
    request.alphaLock = alphaLock;
    for (int ty = 0; ty * kTileSize < h; ++ty) {
        for (int tx = 0; tx * kTileSize < w; ++tx) request.tiles.emplace_back(tx, ty);
    }
    renderer.previewFx(request, lookupIn(base));
    return applied(base, renderer.commitFx(lookupIn(base)));
}

TileMap evenField(int tilesX, int tilesY, float r, float g, float b, float a) {
    TileMap m;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) m[{tx, ty}] = solidTile(r, g, b, a);
    }
    return m;
}

double worstDifference(const TileMap& a, const TileMap& b, int w, int h, int step = 3) {
    double worst = 0.0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) worst = std::max(worst, pixelDistance(docPixel(a, x, y), docPixel(b, x, y)));
    }
    return worst;
}

}  // namespace

TEST_CASE(gaussianKeepsConstantAndNormalises) {
    using chroma::core::FxKind;
    // A constant PMA field stays constant, edges of the document included (the edge pixel extends).
    const TileMap field = evenField(3, 2, 0.4f, 0.7f, 0.2f, 0.6f);
    const TileMap blurred = runFx(chroma::core::defaultFx(FxKind::Gaussian), field, 384, 256);
    CHECK(worstDifference(blurred, field, 384, 256) <= kHalfTolerance);
    // A centred impulse on zeros at R = 3: the weights sum to about 1, symmetric in both axes.
    auto impulse = std::make_shared<chroma::core::TileData>();
    impulse->halfs[(static_cast<std::size_t>(64) * kTileSize + 64) * 4 + 3] = chroma::core::floatToHalf(1.0f);
    TileMap one;
    one[{1, 1}] = TileRenderer::makeVersion(impulse);
    chroma::core::FxSettings fx = chroma::core::defaultFx(FxKind::Gaussian);
    fx.radius = 3;
    const TileMap spread = runFx(fx, one, 384, 384);
    double sum = 0.0;
    for (int y = 192 - 4; y <= 192 + 4; ++y) {
        for (int x = 192 - 4; x <= 192 + 4; ++x) sum += docPixel(spread, x, y).a;
    }
    CHECK_NEAR(sum, 1.0, 0.01);
    for (int d = 1; d <= 3; ++d) {
        const float right = docPixel(spread, 192 + d, 192).a;
        CHECK(right > 0.0f && right == docPixel(spread, 192 - d, 192).a && right == docPixel(spread, 192, 192 + d).a);
    }
    // A transparent field stays (0, 0, 0, 0): no tile appears.
    CHECK(runFx(fx, {}, 256, 256).empty());
}

TEST_CASE(neutralFiltersAreIdentity) {
    using chroma::core::FxKind;
    TileMap image;
    image[{0, 0}] = randomTile(31, 0.8f);
    image[{1, 0}] = randomTile(32, 0.8f);
    // §12.1: LUT identity (continuous), gain = 0, shift = 0 give the original within rounding.
    chroma::core::FxSettings curve = chroma::core::defaultFx(FxKind::ToneCurve);
    chroma::core::FxSettings unsharp = chroma::core::defaultFx(FxKind::Unsharp);
    unsharp.gain = 0.0;
    chroma::core::FxSettings shift = chroma::core::defaultFx(FxKind::Chromatic);
    shift.shiftX = 0.0;
    for (const auto& fx : {curve, unsharp, shift}) {
        const double d = worstDifference(runFx(fx, image, 256, 128), image, 256, 128, 1);
        if (d > 2 * kHalfTolerance) std::printf("    %s: %.5f\n", std::string(chroma::core::fxName(fx.kind)).c_str(), d);
        CHECK(d <= 2 * kHalfTolerance);
    }
    // A tone curve that inverts the master channel inverts straight colour, alpha kept.
    chroma::core::FxSettings invert = curve;
    invert.curve.channels[0] = {{0, 1}, {1, 0}};
    TileMap grey;
    grey[{0, 0}] = solidTile(0.25f, 0.25f, 0.25f, 0.5f);
    const Pixel p = docPixel(runFx(invert, grey, 128, 128), 10, 10);
    CHECK_NEAR(p.a, 0.5, kHalfTolerance);
    CHECK_NEAR(p.r / p.a, 0.75, 2 * kHalfTolerance);
}

TEST_CASE(animeLineExtraction) {
    using chroma::core::FxKind;
    // White, black and transparent at edgeGain 0, lo 0, hi 1, m .5 → alpha 0, 1, 0; RGB zero.
    TileMap image;
    image[{0, 0}] = solidTile(1, 1, 1, 1);
    image[{1, 0}] = solidTile(0, 0, 0, 1);
    chroma::core::FxSettings fx = chroma::core::defaultFx(FxKind::AnimeLines);
    fx.blackPoint = 0.0;
    fx.whitePoint = 1.0;
    fx.mid = 0.5;
    fx.edgeGain = 0.0;
    fx.toNewLayer = false;
    const TileMap lines = runFx(fx, image, 384, 128);
    CHECK(docPixel(lines, 60, 60).a == 0.0f);
    CHECK(docPixel(lines, 190, 60).a == 1.0f && docPixel(lines, 190, 60).r == 0.0f);
    CHECK(docPixel(lines, 320, 60).a == 0.0f);
    // A straight grey 0.25 at a = 1 with m = .25 gives about 0.25.
    TileMap grey;
    grey[{0, 0}] = solidTile(0.25f, 0.25f, 0.25f, 1);
    fx.mid = 0.25;
    CHECK_NEAR(docPixel(runFx(fx, grey, 128, 128), 64, 64).a, 0.25, kHalfTolerance);
}

TEST_CASE(fxSelectionAmountAndLock) {
    using chroma::core::FxKind;
    TileMap red;
    red[{0, 0}] = solidTile(1, 0, 0, 0.5f);
    chroma::core::FxSettings invert = chroma::core::defaultFx(FxKind::ToneCurve);
    invert.curve.channels[0] = {{0, 1}, {1, 0}};
    // The selection weights the final mix: inside inverted, outside the original.
    const chroma::core::SelectionMask left = chroma::core::rasterizeLasso({{0, 0}, {64, 0}, {64, 128}, {0, 128}}, 128, 128);
    const TileMap masked = runFx(invert, red, 128, 128, &left);
    CHECK(docPixel(masked, 10, 10).g > 0.49f && docPixel(masked, 100, 10).g == 0.0f);
    // amount 0.5: halfway.
    invert.amount = 0.5;
    CHECK_NEAR(docPixel(runFx(invert, red, 128, 128), 10, 10).g, 0.25, 2 * kHalfTolerance);
    // Under Alpha Lock the blur keeps every alpha.
    TileMap half;
    half[{0, 0}] = solidTile(1, 0, 0, 1, 0, 64);
    const TileMap blurred = runFx(chroma::core::defaultFx(FxKind::Gaussian), half, 128, 128, nullptr, true);
    CHECK(docPixel(blurred, 70, 10).a == 0.0f && docPixel(blurred, 60, 10).a == 1.0f);
}

// ── The chrome (spec §11.1) ──────────────────────────────────────────────────
// Drawn into the back buffer of a hidden 1440 × 900 window with its own context, read back as RGBA8.
// Set CHROMA_UI_SNAPSHOT to a directory to also get the frames as 24-bit BMP files.

class PreviewHost final : public chroma::ui::UiHost {
public:
    explicit PreviewHost(chroma::ui::BrushPreview& preview) : preview_(preview) { state.layers = &tree; }
    chroma::core::LayerTree tree = chroma::core::LayerTree::single();
    chroma::ui::UiState state;
    void selectTool(chroma::ui::Tool tool) override { state.tool = tool; }
    void selectBrush(std::size_t index) override { state.brush = index; }
    void setDiameter(double px) override { state.diameter = px; }
    void setOpacity(double unit) override { state.opacity = unit; }
    void setStabilizer(double ms) override { state.stabilizerMs = ms; }
    void setTolerance(double unit) override { state.tolerance = unit; }
    void fxChanged(const chroma::core::FxSettings& fx) override {
        lastFx = fx;
        ++fxChanges;
    }
    void fxApply(const chroma::core::FxSettings& fx) override {
        lastFx = fx;
        ++fxApplied;
    }
    void fxCancel() override { ++fxCancelled; }
    chroma::core::FxSettings lastFx;
    int fxChanges = 0, fxApplied = 0, fxCancelled = 0;
    void setColor(const chroma::core::Rgb& srgb, double alpha) override {
        state.color = srgb;
        state.alpha = alpha;
    }
    void selectLayer(std::uint64_t) override {}
    void setLayerFlag(std::uint64_t, chroma::ui::LayerFlag, bool) override {}
    void setLayerOpacity(std::uint64_t, double, bool) override {}
    void setLayerBlend(std::uint64_t, int) override {}
    void renameLayer(std::uint64_t, const std::string&) override {}
    void moveLayer(std::uint64_t, std::uint64_t, std::size_t) override {}
    void run(chroma::ui::Command) override {}
    void drawBrushPreview(chroma::ui::UiRenderer& r, std::size_t index, const chroma::ui::Rect& area) override {
        preview_.draw(r, chroma::core::builtInBrushes()[index], area, state.color, state.alpha);
    }
    void drawLayerThumbnail(chroma::ui::UiRenderer&, std::uint64_t, const chroma::ui::Rect&) override {}

private:
    chroma::ui::BrushPreview& preview_;
};

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // row 0 = top

    [[nodiscard]] std::array<int, 3> at(int x, int y) const {
        const std::size_t i =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
};

Frame readBack(int width, int height) {
    glFinish();
    Frame f{width, height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)};
    std::vector<std::uint8_t> raw(f.rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        std::copy_n(raw.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(height - 1 - y) * row), row,
                    f.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row));
    }
    return f;
}

void saveSnapshot(const Frame& f, const char* name) {
    char* directory = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&directory, &length, "CHROMA_UI_SNAPSHOT") != 0 || !directory) return;
    const std::string path = std::string(directory) + "\\" + name + ".bmp";
    std::free(directory);
    const auto rowBytes = static_cast<std::uint32_t>((f.width * 3 + 3) & ~3);
    const std::uint32_t imageBytes = rowBytes * static_cast<std::uint32_t>(f.height);
    std::ofstream out(path, std::ios::binary);
    const auto u16 = [&out](std::uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };
    const auto u32 = [&u16](std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    };
    out.put('B');
    out.put('M');
    u32(54 + imageBytes);
    u32(0);
    u32(54);
    u32(40);
    u32(static_cast<std::uint32_t>(f.width));
    u32(static_cast<std::uint32_t>(-f.height));  // top-down rows
    u16(1);
    u16(24);
    u32(0);
    u32(imageBytes);
    u32(2835);
    u32(2835);
    u32(0);
    u32(0);
    std::vector<char> line(rowBytes, 0);
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            const auto p = f.at(x, y);
            const std::size_t i = static_cast<std::size_t>(x) * 3;
            line[i] = static_cast<char>(p[2]);
            line[i + 1] = static_cast<char>(p[1]);
            line[i + 2] = static_cast<char>(p[0]);
        }
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
}

bool near8(const std::array<int, 3>& p, const chroma::ui::Color& c, int tolerance = 2) {
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return std::abs(p[0] - byte(c.r)) <= tolerance && std::abs(p[1] - byte(c.g)) <= tolerance &&
           std::abs(p[2] - byte(c.b)) <= tolerance;
}

bool near8(const std::array<int, 3>& p, int r, int g, int b, int tolerance) {
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
}

/// Pixels in [x0, x1) × [y0, y1) that differ from `background` by more than `delta` (sum over RGB).
int inkPixels(const Frame& f, int x0, int y0, int x1, int y1, const std::array<int, 3>& background, int delta) {
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const auto p = f.at(x, y);
            const int d =
                std::abs(p[0] - background[0]) + std::abs(p[1] - background[1]) + std::abs(p[2] - background[2]);
            if (d > delta) ++count;
        }
    }
    return count;
}

TEST_CASE(chromeCatalogAndColourPopups) {
    using namespace chroma;
    constexpr int kWidth = 1440;
    constexpr int kHeight = 900;
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ChromaStudio.GpuTests.Chrome";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    CHECK(window != nullptr);
    if (!window) return;
    std::string failure;
    {
        std::unique_ptr<gl::Context> context = gl::Context::create(window, failure);
        CHECK(context != nullptr);
        if (context) {
            ui::UiRenderer chrome(*context);
            ui::BrushPreview preview(*context);
            PreviewHost host(preview);
            host.state.color = {0, 0, 0};
            ui::EditorUi editor(host);
            editor.resize(kWidth, kHeight);
            const ui::Theme& theme = editor.theme();
            const auto render = [&] {
                chrome.begin(kWidth, kHeight, 1.0);
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // the canvas is the host's; black here
                glClear(GL_COLOR_BUFFER_BIT);
                chrome.restoreState();
                editor.draw(chrome, host.state);
                chrome.restoreState();
                editor.drawOverlays(chrome, host.state);
                return readBack(kWidth, kHeight);
            };
            const auto keyDown = [](std::uint32_t vk) {
                platform::KeyEvent k;
                k.virtualKey = vk;
                k.down = true;
                return k;
            };

            // 1. Brush catalogue (F6) with the real-engine preview of the current brush.
            editor.key(keyDown(VK_F6), host.state);
            const Frame catalog = render();
            saveSnapshot(catalog, "ui-catalog");
            CHECK(near8(catalog.at(1300, 16), theme.menuBar));
            CHECK(near8(catalog.at(800, 892), theme.statusBar));
            CHECK(near8(catalog.at(28, 860), theme.toolbar));
            // Menu titles are text: ink in the "Файл" area.
            CHECK(inkPixels(catalog, 16, 6, 64, 26, catalog.at(4, 16), 120) > 30);
            // Popup (8, 76, 456, 520); preview area (22, 130, 428, 92) on white paper.
            CHECK(near8(catalog.at(440, 140), 255, 255, 255, 2));
            // The natural stamp of "Твёрдое перо" (Ø 12) is black at the centre of the left square.
            CHECK(near8(catalog.at(22 + 46, 130 + 46), 0, 0, 0, 24));
            // The test curve leaves ink across the rest of the preview, and not everywhere.
            const int curveInk = inkPixels(catalog, 22 + 92, 130, 22 + 428, 130 + 92, {255, 255, 255}, 200);
            CHECK(curveInk > 600);
            CHECK(curveInk < 428 * 92 / 2);
            editor.key(keyDown(VK_ESCAPE), host.state);
            CHECK(!editor.overlayOpen());

            // 1b. The watercolor preview: a two-colour ground (§11.1) with pigment carried along the curve.
            host.state.brush = 4;
            host.state.color = {0.1, 0.2, 0.8};
            editor.key(keyDown(VK_F6), host.state);
            const Frame watercolor = render();
            saveSnapshot(watercolor, "ui-watercolor");
            const auto ground = watercolor.at(22 + 120, 130 + 6);  // the cream left half, above the curve
            CHECK(ground[0] > 230 && ground[2] < 215);
            const auto blueGround = watercolor.at(22 + 400, 130 + 6);
            CHECK(blueGround[2] > 230 && blueGround[0] < 200);
            editor.key(keyDown(VK_ESCAPE), host.state);
            host.state.brush = 0;
            host.state.color = {0, 0, 0};

            // 2. HSV popup from the colour swatch: ring centred at (284, 188), S/V square (234, 138, 100, 100).
            platform::PointerSample s;
            s.phase = platform::PointerPhase::Down;
            s.x = s.xPx = 178;
            s.y = s.yPx = 56;
            s.buttons = platform::Button::Primary;
            editor.pointer(s, host.state);
            s.phase = platform::PointerPhase::Up;
            s.buttons = 0;
            editor.pointer(s, host.state);
            CHECK(editor.overlayOpen());
            const Frame colour = render();
            saveSnapshot(colour, "ui-colour");
            // (The markers sit at H = 0 on the ring and at S = V = 0 in the square: black is current.)
            CHECK(near8(colour.at(284, 188 - 87), 128, 255, 0, 12));  // H = 1/4 at the top
            CHECK(near8(colour.at(284 - 87, 188), 0, 255, 255, 12));  // H = 1/2 on the left
            CHECK(near8(colour.at(284, 188 + 87), 128, 0, 255, 12));  // H = 3/4 at the bottom
            // The S/V square of the current hue: S = local x, V = 1 − local y at pixel centres.
            for (const auto& [x, y] : {std::pair{236, 140}, std::pair{283, 140}, std::pair{300, 236}, std::pair{320, 190}}) {
                const double hue = core::rgbToHsv({0.1, 0.2, 0.8}).h;  // §11.1: the last meaningful hue is kept
                const core::Rgb e = core::hsvToRgb({hue, (x + 0.5 - 234) / 100, 1 - (y + 0.5 - 138) / 100});
                CHECK(near8(colour.at(x, y), core::toByte(e[0]), core::toByte(e[1]), core::toByte(e[2]), 2));
            }
            editor.key(keyDown(VK_ESCAPE), host.state);

            // 3. The layer list with a group, and the blend-mode dropdown of the active layer.
            const std::uint64_t group = host.tree.addGroup("Персонаж");
            const std::uint64_t shade = host.tree.addRaster("Тень");
            host.tree.move(shade, group, 0);
            core::LayerProps props = host.tree.find(shade)->props;
            props.blendId = 1;
            props.clipToBase = true;
            host.tree.setProps(shade, props);
            s.phase = platform::PointerPhase::Down;
            s.x = s.xPx = 1160;  // the blend button (1128, 836, 134, 32)
            s.y = s.yPx = 852;
            s.buttons = platform::Button::Primary;
            editor.pointer(s, host.state);
            s.phase = platform::PointerPhase::Up;
            s.buttons = 0;
            editor.pointer(s, host.state);
            CHECK(editor.overlayOpen());
            const Frame layers = render();
            saveSnapshot(layers, "ui-layers");
            editor.key(keyDown(VK_ESCAPE), host.state);

            // 4. The tone-curve dialog (§9.3): Фильтр → Кривые.
            editor.key(keyDown(VK_F10), host.state);
            for (int i = 0; i < 5; ++i) editor.key(keyDown(VK_RIGHT), host.state);
            editor.key(keyDown(VK_DOWN), host.state);
            editor.key(keyDown(VK_RETURN), host.state);
            CHECK(editor.overlayOpen() && host.fxChanges == 1 && host.lastFx.kind == core::FxKind::ToneCurve);
            const Frame curves = render();
            saveSnapshot(curves, "ui-curves");
            editor.key(keyDown(VK_ESCAPE), host.state);
            CHECK(host.fxCancelled == 1);
            // Rows of the list: the group at the top, its child indented, then «Слой 1».
            CHECK(inkPixels(layers, 1220, 160, 1320, 192, layers.at(1430, 175), 120) > 20);  // «Персонаж»
            CHECK(near8(layers.at(1395, 203), theme.selection, 3));  // the active row: «Тень» (row 1)
        }
    }
    DestroyWindow(window);
    g_context->makeCurrent();
}

int main() {
    chroma::setLogSink([](std::string_view line) {
        std::printf("    log: %.*s\n", static_cast<int>(line.size()), line.data());
    });
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ChromaStudio.GpuTests";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    std::string failure = "cannot create a window";
    std::unique_ptr<chroma::gl::Context> context = window ? chroma::gl::Context::create(window, failure) : nullptr;
    if (!context) {
        std::printf("SKIP: no OpenGL 3.3 context: %s\n", failure.c_str());
        if (window) DestroyWindow(window);
        return 77;
    }
    g_context = context.get();
    const int result = test::runAll();
    g_context = nullptr;
    context.reset();
    DestroyWindow(window);
    return result;
}

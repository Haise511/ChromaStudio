// ChromaStudio Engine · ui_tests: layout (§11.1), sliders, popups, HSV/HEX, the preview curve, layer
// metadata in the history (§10.2) and the chrome's input handling (§11.2) against a recording host.
// No window or GL context is needed: EditorUi is driven with synthetic events.

#include "TestHarness.hpp"

#include "core/Color.hpp"
#include "core/Layer.hpp"
#include "core/Fx.hpp"
#include "core/TileHistory.hpp"
#include "ui/EditorUi.hpp"
#include "ui/Layout.hpp"

#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace chroma;
using ui::Rect;

void checkRect(const Rect& actual, const Rect& expected, const char* what) {
    if (!(std::abs(actual.x - expected.x) < 1e-9 && std::abs(actual.y - expected.y) < 1e-9 &&
          std::abs(actual.w - expected.w) < 1e-9 && std::abs(actual.h - expected.h) < 1e-9)) {
        ::test::fail(__FILE__, __LINE__,
                     std::format("{} = ({}, {}, {}, {}), expected ({}, {}, {}, {})", what, actual.x, actual.y, actual.w,
                                 actual.h, expected.x, expected.y, expected.w, expected.h));
    }
}

// ── Layout ───────────────────────────────────────────────────────────────────

TEST_CASE(standardLayout1440x900) {
    ui::LayoutInput in;
    in.width = 1440;
    in.height = 900;
    in.dockShown = ui::defaultDockShown(in.width);
    const ui::ShellLayout l = ui::computeShellLayout(in);
    CHECK(ui::defaultDockWidth(1440) == 320);  // round(2·1440/9)
    checkRect(l.menu, {0, 0, 1440, 32}, "menu");
    checkRect(l.options, {0, 32, 1440, 48}, "options");
    checkRect(l.toolbar, {0, 80, 56, 796}, "toolbar");
    checkRect(l.canvas, {56, 80, 1060, 796}, "canvas");
    checkRect(l.splitter, {1116, 80, 4, 796}, "splitter");
    checkRect(l.dock, {1120, 80, 320, 796}, "dock");
    checkRect(l.status, {0, 876, 1440, 24}, "status");
    CHECK(l.dockVisible && l.optionsVisible && l.toolbarVisible);
    CHECK(!l.narrow && !l.compact && !l.shortDock);
    checkRect(l.profile, {8, 40, 144, 32}, "profile");
    checkRect(l.color, {160, 40, 36, 32}, "color");
    checkRect(l.size, {208, 40, 240, 32}, "size");
    checkRect(l.opacity, {464, 40, 224, 32}, "opacity");
    checkRect(l.stabilizer, {704, 40, 176, 32}, "stabilizer");
    CHECK(l.more.empty());
    checkRect(l.tools[0], {8, 88, 40, 40}, "tool 0");
    checkRect(l.tools[10], {8, 88 + 440, 40, 40}, "tool 10");
    CHECK(l.maxToolScroll == 0);
    // Dock interior: tabs 36, actions 36, properties 88, list between.
    checkRect(l.dockTabs, {1120, 80, 320, 36}, "dock tabs");
    checkRect(l.dockActions, {1120, 116, 320, 36}, "dock actions");
    checkRect(l.dockProperties, {1120, 788, 320, 88}, "dock properties");
    checkRect(l.dockList, {1120, 152, 320, 636}, "dock list");
}

// §12.1 "Окно": the 1440 × 900 DIP window at 144 DPI has a canvas viewport 1590 physical px wide, and
// at 96 DPI exactly its DIP size; neighbouring regions share their rounded edges.
TEST_CASE(viewportAt144Dpi) {
    ui::LayoutInput in;
    in.width = 1440;
    in.height = 900;
    in.dockShown = ui::defaultDockShown(in.width);
    const ui::ShellLayout l = ui::computeShellLayout(in);
    const ui::EdgesPx canvas = ui::physicalEdges(l.canvas, 144.0 / 96.0);
    CHECK(canvas.x1 - canvas.x0 == 1590);
    CHECK(canvas.y1 - canvas.y0 == 1194);
    CHECK(ui::physicalEdges(l.toolbar, 1.5).x1 == canvas.x0 && ui::physicalEdges(l.splitter, 1.5).x0 == canvas.x1);
    const ui::EdgesPx at96 = ui::physicalEdges(l.canvas, 1.0);
    CHECK(at96.x1 - at96.x0 == 1060 && at96.y1 - at96.y0 == 796);
    // 125 % and 175 %: shared edges, no gaps.
    for (const double scale : {1.25, 1.75}) {
        CHECK(ui::physicalEdges(l.toolbar, scale).x1 == ui::physicalEdges(l.canvas, scale).x0);
        CHECK(ui::physicalEdges(l.options, scale).y1 == ui::physicalEdges(l.canvas, scale).y0);
    }
}

TEST_CASE(dockWidthFormulaAndLimits) {
    CHECK(ui::defaultDockWidth(1000) == 264);  // 222 → clamped up
    CHECK(ui::defaultDockWidth(1800) == 400);
    CHECK(ui::defaultDockWidth(2400) == 400);  // 533 → clamped down
    ui::LayoutInput in;
    in.width = 1440;
    in.height = 900;
    in.dockWidth = 1000;  // dragged too far: clamped to 400
    ui::ShellLayout l = ui::computeShellLayout(in);
    checkRect(l.dock, {1040, 80, 400, 796}, "dock at max");
    in.dockWidth = 100;
    l = ui::computeShellLayout(in);
    checkRect(l.dock, {1176, 80, 264, 796}, "dock at min");
    checkRect(l.canvas, {56, 80, 1116, 796}, "canvas next to min dock");
}

TEST_CASE(narrowLayout900) {
    ui::LayoutInput in;
    in.width = 900;
    in.height = 700;
    in.dockShown = ui::defaultDockShown(in.width);
    CHECK(!in.dockShown);
    const ui::ShellLayout l = ui::computeShellLayout(in);
    CHECK(l.narrow && !l.compact && !l.dockVisible);
    checkRect(l.canvas, {56, 80, 844, 596}, "canvas");
    CHECK(l.stabilizer.empty());
    checkRect(l.more, {704, 40, 32, 32}, "more");
    // F7 can still show the dock: R = 264, canvas 900 − 264 − 60 = 576 ≥ 200.
    in.dockShown = true;
    const ui::ShellLayout shown = ui::computeShellLayout(in);
    CHECK(shown.dockVisible);
    checkRect(shown.canvas, {56, 80, 576, 596}, "canvas with dock");
}

TEST_CASE(compactLayout700) {
    ui::LayoutInput in;
    in.width = 700;
    in.height = 500;
    in.dockShown = true;  // asked for, but the canvas would be 700 − 264 − 60 = 376 ≥ 200: shown
    ui::ShellLayout l = ui::computeShellLayout(in);
    CHECK(l.compact && l.narrow && l.shortDock);
    checkRect(l.profile, {8, 40, 104, 32}, "profile");
    checkRect(l.color, {120, 40, 32, 32}, "color");
    checkRect(l.size, {160, 40, 184, 32}, "size");
    checkRect(l.opacity, {352, 40, 184, 32}, "opacity");
    checkRect(l.more, {544, 40, 32, 32}, "more");
    // Toolbar 396 high, content 8 + 44·11 = 492: scrolls by up to 96.
    CHECK_NEAR(l.maxToolScroll, 96, 1e-9);
    in.toolScroll = 500;
    l = ui::computeShellLayout(in);
    CHECK_NEAR(l.toolScroll, 96, 1e-9);
    checkRect(l.tools[0], {8, -8, 40, 40}, "tool 0 scrolled");
    // Too narrow for a dock: hidden even when asked for.
    in.width = 500;
    l = ui::computeShellLayout(in);
    CHECK(!l.dockVisible);
    checkRect(l.canvas, {56, 80, 444, 396}, "canvas");
}

TEST_CASE(panelsHiddenWithTab) {
    ui::LayoutInput in;
    in.width = 1440;
    in.height = 900;
    in.panelsHidden = true;
    const ui::ShellLayout l = ui::computeShellLayout(in);
    CHECK(!l.optionsVisible && !l.toolbarVisible && !l.dockVisible);
    checkRect(l.canvas, {0, 32, 1440, 844}, "canvas");
    CHECK(l.profile.empty() && l.size.empty());
}

// ── Sliders and popups ───────────────────────────────────────────────────────

TEST_CASE(sliderGeometryAndMapping) {
    const ui::SliderGeometry g = ui::sliderGeometry({208, 40, 240, 32});
    CHECK(g.x0 == 216);
    CHECK(g.x1 == 380);
    checkRect(g.value, {388, 42, 60, 28}, "value");
    checkRect(g.track, {216, 54, 164, 4}, "track");
    CHECK(ui::sliderT(g, 100) == 0.0);
    CHECK(ui::sliderT(g, 500) == 1.0);
    CHECK_NEAR(ui::sliderT(g, 298), 0.5, 1e-12);
    CHECK_NEAR(ui::diameterFromT(0), 0.25, 1e-12);
    CHECK_NEAR(ui::diameterFromT(1), 2048, 1e-9);
    CHECK_NEAR(ui::diameterFromT(0.5), 0.25 * std::sqrt(8192.0), 1e-9);
    for (const double d : {0.25, 1.0, 12.0, 100.0, 2048.0}) CHECK_NEAR(ui::diameterFromT(ui::tFromDiameter(d)), d, 1e-9 * d);
    CHECK(ui::tFromDiameter(0.01) == 0.0);
    CHECK(ui::tFromDiameter(1e6) == 1.0);
}

TEST_CASE(popupPlacement) {
    // Below the anchor when it fits.
    checkRect(ui::placePopup({160, 40, 36, 32}, 248, 280, 1440, 900), {160, 76, 248, 280}, "below");
    // Above the anchor when the bottom would cross H − 8.
    checkRect(ui::placePopup({160, 700, 36, 32}, 248, 280, 1440, 900), {160, 416, 248, 280}, "above");
    // Clamped into [8, W − pw − 8].
    checkRect(ui::placePopup({1400, 40, 36, 32}, 248, 280, 1440, 900), {1184, 76, 248, 280}, "right edge");
    checkRect(ui::placePopup({-50, 40, 36, 32}, 248, 280, 1440, 900), {8, 76, 248, 280}, "left edge");
    // The catalogue size and interior.
    checkRect(ui::catalogSize(1440, 900), {0, 0, 456, 520}, "catalogue");
    checkRect(ui::catalogSize(400, 500), {0, 0, 384, 404}, "small catalogue");
    const ui::CatalogLayout c = ui::catalogLayout({8, 76, 456, 520});
    checkRect(c.search, {20, 88, 432, 32}, "search");
    checkRect(c.preview, {20, 128, 432, 96}, "preview");
    checkRect(c.categories, {20, 232, 112, 352}, "categories");
    checkRect(c.list, {140, 232, 312, 352}, "list");
}

TEST_CASE(previewCurveNodes) {
    const auto nodes = ui::previewCurve(300, 96);
    CHECK(nodes.size() == 65);
    CHECK_NEAR(nodes.front().x, 12, 1e-12);
    CHECK_NEAR(nodes.back().x, 288, 1e-9);
    CHECK_NEAR(nodes.front().y, 48, 1e-12);
    CHECK_NEAR(nodes[16].y, 48 + 0.22 * 96, 1e-9);  // t = 1/4: the crest
    CHECK_NEAR(nodes.front().pressure, 0.15, 1e-12);
    CHECK_NEAR(nodes[32].pressure, 1.0, 1e-12);
    CHECK_NEAR(nodes.back().pressure, 0.15, 1e-9);
    for (std::size_t i = 1; i < nodes.size(); ++i) CHECK_NEAR(nodes[i].timeMs - nodes[i - 1].timeMs, 8.0, 1e-12);
}

// ── Colour ───────────────────────────────────────────────────────────────────

TEST_CASE(hsvSectorsAndRoundTrip) {
    const auto rgbNear = [](const core::Rgb& a, const core::Rgb& b) {
        return std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9 && std::abs(a[2] - b[2]) < 1e-9;
    };
    CHECK(rgbNear(core::hsvToRgb({0, 1, 1}), {1, 0, 0}));
    CHECK(rgbNear(core::hsvToRgb({1.0 / 6, 1, 1}), {1, 1, 0}));
    CHECK(rgbNear(core::hsvToRgb({1.0 / 3, 1, 1}), {0, 1, 0}));
    CHECK(rgbNear(core::hsvToRgb({0.5, 1, 1}), {0, 1, 1}));
    CHECK(rgbNear(core::hsvToRgb({2.0 / 3, 1, 1}), {0, 0, 1}));
    CHECK(rgbNear(core::hsvToRgb({5.0 / 6, 1, 1}), {1, 0, 1}));
    CHECK(rgbNear(core::hsvToRgb({1.0, 1, 1}), {1, 0, 0}));  // H wraps
    CHECK(rgbNear(core::hsvToRgb({0.3, 0, 0.5}), {0.5, 0.5, 0.5}));
    // A grey keeps the remembered hue (§11.1).
    const core::Hsv grey = core::rgbToHsv({0.4, 0.4, 0.4}, 0.7);
    CHECK_NEAR(grey.h, 0.7, 1e-12);
    CHECK_NEAR(grey.s, 0.0, 1e-12);
    CHECK_NEAR(grey.v, 0.4, 1e-12);
    for (int hi = 0; hi < 24; ++hi) {
        for (const double s : {0.25, 0.5, 1.0}) {
            for (const double v : {0.2, 0.6, 1.0}) {
                const core::Hsv in{hi / 24.0, s, v};
                const core::Hsv out = core::rgbToHsv(core::hsvToRgb(in));
                CHECK_NEAR(out.h, in.h, 1e-9);
                CHECK_NEAR(out.s, in.s, 1e-9);
                CHECK_NEAR(out.v, in.v, 1e-9);
            }
        }
    }
}

TEST_CASE(hueRingAngles) {
    CHECK_NEAR(core::hueFromOffset(1, 0), 0.0, 1e-12);   // right: red
    CHECK_NEAR(core::hueFromOffset(0, -1), 0.25, 1e-12);  // up (screen y grows down)
    CHECK_NEAR(core::hueFromOffset(-1, 0), 0.5, 1e-12);
    CHECK_NEAR(core::hueFromOffset(0, 1), 0.75, 1e-12);
    CHECK_NEAR(core::hueFromOffset(1, 1e-12), 1.0, 1e-9);  // just below the axis wraps towards 1
}

TEST_CASE(hexFormatAndParse) {
    CHECK(core::toHex({1, 0.5, 0}) == "#FF8000");
    CHECK(core::toHex({0, 0, 0}) == "#000000");
    CHECK(core::toHex({2, -1, 1}) == "#FF00FF");  // clamped
    const auto a = core::parseHex("#ff8000");
    CHECK(a && std::abs((*a)[0] - 1) < 1e-12 && std::abs((*a)[1] - 128 / 255.0) < 1e-12 && (*a)[2] == 0);
    const auto b = core::parseHex("0A0B0C");
    CHECK(b && std::abs((*b)[2] - 12 / 255.0) < 1e-12);
    const auto c = core::parseHex("#F0a");
    CHECK(c && (*c)[0] == 1.0 && (*c)[1] == 0.0 && (*c)[2] == 170 / 255.0);
    CHECK(!core::parseHex(""));
    CHECK(!core::parseHex("#12345"));
    CHECK(!core::parseHex("#GG0000"));
    CHECK(!core::parseHex("##123"));
}

// ── Layer metadata in the history (§10.2) ────────────────────────────────────

TEST_CASE(layerPropsTravelWithCommands) {
    using Ref = std::shared_ptr<const int>;
    core::TileHistory<Ref, core::LayerProps> history(1 << 20, 4);
    core::LayerProps base;
    base.name = "Фон";
    history.replaceAll({}, base);
    const std::uint64_t clean = history.revision();
    CHECK(history.meta().name == "Фон");

    core::LayerProps hidden = base;
    hidden.visible = false;
    CHECK(history.commit({}, hidden));       // a metadata-only command
    CHECK(!history.commit({}, hidden));      // same value: nothing to commit
    CHECK(!history.meta().visible);
    core::LayerProps faded = hidden;
    faded.opacity = 0.4;
    const core::TileKey key{1, 0, 0};
    CHECK(history.commit({{key, nullptr, std::make_shared<const int>(7)}}, faded));  // tiles + meta together
    CHECK(history.size() == 2);
    CHECK(history.revision() != clean);

    CHECK(history.undo() != nullptr);
    CHECK(history.meta().opacity == 1.0 && !history.meta().visible && !history.get(key));
    CHECK(history.undo() != nullptr);
    CHECK(history.meta() == base);
    CHECK(history.revision() == clean);  // back at the saved state: not dirty
    CHECK(history.redo() != nullptr);
    CHECK(history.redo() != nullptr);
    CHECK(history.meta() == faded && history.get(key) && *history.get(key) == 7);

    // A tile-only command leaves the metadata alone, and its undo does too.
    CHECK(history.commit({{key, history.get(key), std::make_shared<const int>(8)}}));
    CHECK(history.undo() != nullptr);
    CHECK(history.meta() == faded);
}

// ── EditorUi input against a recording host ──────────────────────────────────

class RecordingHost final : public ui::UiHost {
public:
    RecordingHost() { state.layers = &tree; }
    core::LayerTree tree = core::LayerTree::single();
    ui::UiState state;
    std::vector<ui::Command> commands;
    std::vector<std::pair<double, bool>> layerOpacity;
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::size_t>> moves;
    int brushSelections = 0;

    core::LayerProps props(std::uint64_t id) const { return tree.find(id)->props; }
    void selectTool(ui::Tool tool) override { state.tool = tool; }
    void selectBrush(std::size_t index) override {
        state.brush = index;
        ++brushSelections;
    }
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
    void setColor(const core::Rgb& srgb, double alpha) override {
        state.color = srgb;
        state.alpha = alpha;
    }
    void selectLayer(std::uint64_t id) override { tree.setActive(id); }
    void setLayerFlag(std::uint64_t id, ui::LayerFlag flag, bool on) override {
        core::LayerProps p = props(id);
        switch (flag) {
        case ui::LayerFlag::Visible: p.visible = on; break;
        case ui::LayerFlag::AlphaLock: p.alphaLock = on; break;
        case ui::LayerFlag::ClipToBase: p.clipToBase = on; break;
        case ui::LayerFlag::Locked: p.locked = on; break;
        }
        tree.setProps(id, p);
    }
    void setLayerOpacity(std::uint64_t id, double opacity, bool final) override {
        layerOpacity.emplace_back(opacity, final);
        if (!final) return;
        core::LayerProps p = props(id);
        p.opacity = opacity;
        tree.setProps(id, p);
    }
    void setLayerBlend(std::uint64_t id, int mode) override {
        core::LayerProps p = props(id);
        p.blendId = mode;
        tree.setProps(id, p);
    }
    void renameLayer(std::uint64_t id, const std::string& utf8) override {
        core::LayerProps p = props(id);
        p.name = utf8;
        tree.setProps(id, p);
    }
    void moveLayer(std::uint64_t id, std::uint64_t parent, std::size_t index) override {
        moves.emplace_back(id, parent, index);
        tree.move(id, parent, index);
    }
    void run(ui::Command command) override {
        commands.push_back(command);
        if (command == ui::Command::NewLayer) tree.addRaster();
        if (command == ui::Command::NewGroup) tree.addGroup();
    }
    void drawBrushPreview(ui::UiRenderer& r, std::size_t, const ui::Rect&) override { r.restoreState(); }
    void drawLayerThumbnail(ui::UiRenderer&, std::uint64_t, const ui::Rect&) override {}
};

double g_clock = 0.0;  // one second between samples: no accidental double clicks

platform::PointerSample mouse(platform::PointerPhase phase, double x, double y, bool pressed = true) {
    platform::PointerSample s;
    s.timeSeconds = (g_clock += 1.0);
    s.kind = platform::PointerKind::Mouse;
    s.phase = phase;
    s.x = s.xPx = x;
    s.y = s.yPx = y;
    s.buttons = pressed ? platform::Button::Primary : 0;
    return s;
}

platform::KeyEvent press(std::uint32_t vk, bool shift = false) {
    platform::KeyEvent k;
    k.virtualKey = vk;
    k.down = true;
    k.shift = shift;
    return k;
}

void type(ui::EditorUi& ui, RecordingHost& host, std::u32string_view text) {
    for (const char32_t c : text) ui.character(c, host.state);
}

void click(ui::EditorUi& ui, RecordingHost& host, double x, double y) {
    ui.pointer(mouse(platform::PointerPhase::Down, x, y), host.state);
    ui.pointer(mouse(platform::PointerPhase::Up, x, y, false), host.state);
}

TEST_CASE(toolbarSelectsOnlyWorkingTools) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    click(ui, host, 28, 88 + 44 * 10 + 20);  // Hand
    CHECK(host.state.tool == ui::Tool::Hand);
    click(ui, host, 28, 88 + 44 * 9 + 20);  // Pipette
    CHECK(host.state.tool == ui::Tool::Pipette);
    click(ui, host, 28, 88 + 44 * 6 + 20);  // vector nodes
    CHECK(host.state.tool == ui::Tool::Vector);
    for (int i = 0; i < ui::kToolCount; ++i) CHECK(ui::toolAvailable(static_cast<ui::Tool>(i)));  // all eleven work
    // The canvas is not the chrome's.
    CHECK(!ui.pointer(mouse(platform::PointerPhase::Down, 500, 400), host.state));
}

TEST_CASE(sliderDragAndEscapeRestores) {
    RecordingHost host;
    host.state.diameter = 12;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    // Down at the middle of the size track: t = 0.5.
    CHECK(ui.pointer(mouse(platform::PointerPhase::Down, 298, 56), host.state));
    CHECK_NEAR(host.state.diameter, ui::diameterFromT(0.5), 1e-9);
    CHECK(ui.pointer(mouse(platform::PointerPhase::Move, 380, 56), host.state));
    CHECK_NEAR(host.state.diameter, 2048, 1e-9);
    // Esc restores the snapshot (§11.2); the rest of the contact is still swallowed.
    CHECK(ui.key(press(VK_ESCAPE), host.state));
    CHECK_NEAR(host.state.diameter, 12, 1e-12);
    CHECK(ui.pointer(mouse(platform::PointerPhase::Move, 300, 56), host.state));
    CHECK_NEAR(host.state.diameter, 12, 1e-12);
    CHECK(ui.pointer(mouse(platform::PointerPhase::Up, 300, 56, false), host.state));
    CHECK(!ui.pointer(mouse(platform::PointerPhase::Hover, 300, 56, false), host.state));
    // Opacity slider: an Up commits the dragged value.
    ui.pointer(mouse(platform::PointerPhase::Down, 472, 56), host.state);
    ui.pointer(mouse(platform::PointerPhase::Up, 472, 56, false), host.state);
    CHECK_NEAR(host.state.opacity, 0.0, 1e-12);
}

TEST_CASE(typedValueIsClampedAndEscKeepsOld) {
    RecordingHost host;
    host.state.diameter = 12;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    click(ui, host, 418, 56);  // the size value field
    type(ui, host, U"5000");
    CHECK(ui.key(press('B'), host.state));  // tool shortcuts are suppressed while typing
    CHECK(host.state.tool == ui::Tool::Brush);
    ui.key(press(VK_RETURN), host.state);
    CHECK_NEAR(host.state.diameter, 2048, 1e-12);  // clamped
    click(ui, host, 418, 56);
    type(ui, host, U"3,5");
    ui.key(press(VK_RETURN), host.state);
    CHECK_NEAR(host.state.diameter, 3.5, 1e-12);  // comma as decimal separator
    click(ui, host, 418, 56);
    type(ui, host, U"99");
    ui.key(press(VK_ESCAPE), host.state);
    CHECK_NEAR(host.state.diameter, 3.5, 1e-12);
}

TEST_CASE(colorPopupEscRestoresAndClickOutsideIsSwallowed) {
    RecordingHost host;
    host.state.color = {0.2, 0.4, 0.6};
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    click(ui, host, 178, 56);  // colour swatch → HSV popup at (160, 76)
    CHECK(ui.overlayOpen());
    // A click on the ring at the right of the centre picks red-ish hue H = 0.
    const double cx = 160 + ui::kRingCenterX;
    const double cy = 76 + ui::kRingCenterY;
    click(ui, host, cx + 87, cy);
    const core::Hsv picked = core::rgbToHsv(host.state.color);
    CHECK_NEAR(picked.h, 0.0, 1e-9);
    // Esc closes and restores the colour from the moment of opening (§11.1).
    ui.key(press(VK_ESCAPE), host.state);
    CHECK(!ui.overlayOpen());
    CHECK_NEAR(host.state.color[2], 0.6, 1e-12);
    // Reopen; the first click outside closes and does not reach the canvas.
    click(ui, host, 178, 56);
    CHECK(ui.overlayOpen());
    CHECK(ui.pointer(mouse(platform::PointerPhase::Down, 700, 600), host.state));
    CHECK(!ui.overlayOpen());
    ui.pointer(mouse(platform::PointerPhase::Up, 700, 600, false), host.state);
    CHECK(!ui.pointer(mouse(platform::PointerPhase::Down, 700, 600), host.state));
}

TEST_CASE(catalogSearchAndEnterSelects) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    CHECK(ui.key(press(VK_F6), host.state));
    CHECK(ui.overlayOpen());
    type(ui, host, U"zzzz");  // nothing matches
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.brushSelections == 0 && ui.overlayOpen());
    for (int i = 0; i < 4; ++i) ui.key(press(VK_BACK), host.state);
    ui.key(press(VK_DOWN), host.state);  // highlight the second preset
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.brushSelections == 1 && host.state.brush == 1);
    CHECK(!ui.overlayOpen());
}

TEST_CASE(keysToggleDockPanelsAndMenu) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    CHECK(ui.layout().dockVisible);
    ui.key(press(VK_F7), host.state);
    CHECK(!ui.layout().dockVisible);
    checkRect(ui.layout().canvas, {56, 80, 1384, 796}, "canvas without dock");
    ui.key(press(VK_F7), host.state);
    CHECK(ui.layout().dockVisible);
    // Tab with the focus on the canvas hides the panels until pressed again.
    ui.key(press(VK_TAB), host.state);
    CHECK(!ui.layout().toolbarVisible && !ui.layout().dockVisible);
    ui.key(press(VK_TAB), host.state);
    CHECK(ui.layout().toolbarVisible && ui.layout().dockVisible);
    // F10 opens "Файл"; Enter on the first entry runs "Новый".
    ui.key(press(VK_F10), host.state);
    CHECK(ui.overlayOpen());
    ui.key(press(VK_RETURN), host.state);
    CHECK(!ui.overlayOpen());
    CHECK(host.commands.size() == 1 && host.commands[0] == ui::Command::NewDocument);
    // "Правка → Отменить" is disabled without history.
    ui.key(press(VK_F10), host.state);
    ui.key(press(VK_RIGHT), host.state);
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.commands.size() == 1 && ui.overlayOpen());
    host.state.canUndo = true;
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.commands.size() == 2 && host.commands[1] == ui::Command::Undo);
}

TEST_CASE(layerOpacityDragIsOneCommand) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    // Properties at (1120, 788, 320, 88); the slider control is (1128, 796, 304, 32).
    const ui::SliderGeometry g = ui::sliderGeometry({1128, 796, 304, 32});
    ui.pointer(mouse(platform::PointerPhase::Down, g.x0, 812), host.state);
    ui.pointer(mouse(platform::PointerPhase::Move, (g.x0 + g.x1) / 2, 812), host.state);
    ui.pointer(mouse(platform::PointerPhase::Up, (g.x0 + g.x1) / 2, 812, false), host.state);
    std::size_t finals = 0;
    for (const auto& [value, final] : host.layerOpacity) finals += final ? 1 : 0;
    CHECK(finals == 1);
    CHECK(host.layerOpacity.back().second);
    CHECK_NEAR(host.props(1).opacity, 0.5, 1e-12);
    // Visibility toggles from the eye in the layer row.
    click(ui, host, 1120 + 4 + 12, 152 + 4 + 20);
    CHECK(!host.props(1).visible);
}

// ── The layer list of the dock (§11.1) ───────────────────────────────────────
// Dock (1120, 80, 320, 796): actions at y 116, the list (1120, 152, 320, 636), rows of 40 from y 156,
// properties (1120, 788, 320, 88).

double rowY(int row) { return 156.0 + 40.0 * row + 19.0; }

TEST_CASE(layerListSelectAndToggle) {
    RecordingHost host;
    host.tree.addRaster();                             // 2
    const std::uint64_t g = host.tree.addGroup();      // 3
    const std::uint64_t inner = host.tree.addRaster();  // 4
    host.tree.move(inner, g, 0);
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    // Rows, top of the stack first: group 3, its child 4, then 2 and 1.
    click(ui, host, 1300, rowY(3));
    CHECK(host.tree.active() == 1);
    click(ui, host, 1300, rowY(1));
    CHECK(host.tree.active() == inner);
    click(ui, host, 1136, rowY(2));  // the eye of layer 2
    CHECK(!host.props(2).visible);
    click(ui, host, 1400, rowY(2));  // the lock slot
    CHECK(host.props(2).locked);
    click(ui, host, 1425, rowY(3));  // the clip slot of layer 1
    CHECK(host.props(1).clipToBase);
    // The chevron collapses the group: its child leaves the list.
    click(ui, host, 1162, rowY(0));
    click(ui, host, 1300, rowY(1));
    CHECK(host.tree.active() == 2);
    // The actions: a new layer.
    click(ui, host, 1142, 134);
    CHECK(host.commands.size() == 1 && host.commands[0] == ui::Command::NewLayer && host.tree.size() == 5);
}

TEST_CASE(layerDragAndDrop) {
    RecordingHost host;
    host.tree.addRaster();  // 2
    host.tree.addRaster();  // 3; rows: 3, 2, 1
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    // Layer 1 (bottom row) dragged onto the upper half of the top row: it becomes the top layer.
    ui.pointer(mouse(platform::PointerPhase::Down, 1300, rowY(2)), host.state);
    ui.pointer(mouse(platform::PointerPhase::Move, 1300, rowY(2) - 2), host.state);  // under 4 DIP: no drag yet
    CHECK(ui.cursorAt(1300, rowY(2)) != ui::CursorKind::Move);
    ui.pointer(mouse(platform::PointerPhase::Move, 1300, 161), host.state);
    CHECK(ui.cursorAt(1300, 161) == ui::CursorKind::Move);
    ui.pointer(mouse(platform::PointerPhase::Up, 1300, 161, false), host.state);
    CHECK(host.moves.size() == 1 && host.moves[0] == std::make_tuple(std::uint64_t{1}, std::uint64_t{0}, std::size_t{2}));
    CHECK(host.tree.children(0) == (std::vector<std::uint64_t>{2, 3, 1}));
    // Esc cancels a reorder in progress (§11.2).
    ui.pointer(mouse(platform::PointerPhase::Down, 1300, rowY(0)), host.state);
    ui.pointer(mouse(platform::PointerPhase::Move, 1300, rowY(2) + 10), host.state);
    CHECK(ui.key(press(VK_ESCAPE), host.state));
    ui.pointer(mouse(platform::PointerPhase::Up, 1300, rowY(2) + 10, false), host.state);
    CHECK(host.moves.size() == 1);
    // Onto the middle of a group row: into the group.
    host.tree.setActive(1);
    const std::uint64_t g = host.tree.addGroup();  // rows: g, 1, 3, 2
    ui.pointer(mouse(platform::PointerPhase::Down, 1300, rowY(3)), host.state);
    ui.pointer(mouse(platform::PointerPhase::Move, 1300, rowY(0) + 1), host.state);
    ui.pointer(mouse(platform::PointerPhase::Up, 1300, rowY(0) + 1, false), host.state);
    CHECK(host.tree.children(g) == (std::vector<std::uint64_t>{2}));
    // A group never goes into itself: dropping it on its own child changes nothing.
    const std::size_t moves = host.moves.size();
    ui.pointer(mouse(platform::PointerPhase::Down, 1300, rowY(0)), host.state);  // the group
    ui.pointer(mouse(platform::PointerPhase::Move, 1300, rowY(1) + 1), host.state);  // its child, middle
    ui.pointer(mouse(platform::PointerPhase::Up, 1300, rowY(1) + 1, false), host.state);
    CHECK(host.moves.size() == moves);
}

TEST_CASE(layerRenameBlendAndChecks) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    // A double click on the name renames; Enter confirms.
    for (int i = 0; i < 2; ++i) {
        platform::PointerSample s = mouse(platform::PointerPhase::Down, 1300, rowY(0));
        s.timeSeconds = 100.0 + 0.1 * i;
        ui.pointer(s, host.state);
        s.phase = platform::PointerPhase::Up;
        s.buttons = 0;
        ui.pointer(s, host.state);
    }
    type(ui, host, U"Фон");
    CHECK(ui.key(press('B'), host.state));  // typing swallows the shortcuts
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.props(1).name == "Фон");
    // Blend modes: the dropdown above the button (1128, 836, 134, 32), Multiply in column 0, row 1.
    click(ui, host, 1160, 852);
    CHECK(ui.overlayOpen());
    click(ui, host, 1100, 582 + 8 + 26 + 12);
    CHECK(host.props(1).blendId == 1 && !ui.overlayOpen());
    // From the keyboard: Down, Down, Right → 1 + 2 + 9 = 12 (Soft Light).
    click(ui, host, 1160, 852);
    ui.key(press(VK_DOWN), host.state);
    ui.key(press(VK_DOWN), host.state);
    ui.key(press(VK_RIGHT), host.state);
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.props(1).blendId == 12 && !ui.overlayOpen());
    // Alpha Lock and clip check boxes.
    click(ui, host, 1280, 852);
    CHECK(host.props(1).alphaLock);
    click(ui, host, 1365, 852);
    CHECK(host.props(1).clipToBase);
}

TEST_CASE(layerListKeyboard) {
    RecordingHost host;
    host.tree.addRaster();
    host.tree.addRaster();  // rows 3, 2, 1; active 3
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    click(ui, host, 1300, rowY(0));  // focus on the list
    ui.key(press(VK_DOWN), host.state);
    CHECK(host.tree.active() == 2);
    ui.key(press(VK_DOWN), host.state);
    ui.key(press(VK_DOWN), host.state);  // stays on the last row
    CHECK(host.tree.active() == 1);
    ui.key(press(VK_UP), host.state);
    CHECK(host.tree.active() == 2);
    ui.key(press(VK_SPACE), host.state);
    CHECK(!host.props(2).visible);
    ui.key(press(VK_F2), host.state);
    type(ui, host, U"Тени");
    ui.key(press(VK_RETURN), host.state);
    CHECK(host.props(2).name == "Тени");
    // Alt+↑ belongs to the host (moving the layer), even with the focus on the list.
    platform::KeyEvent altUp = press(VK_UP);
    altUp.alt = true;
    CHECK(!ui.key(altUp, host.state));
    CHECK(host.tree.active() == 2);
    // Esc returns the focus to the canvas: the arrows no longer move the selection.
    ui.key(press(VK_ESCAPE), host.state);
    CHECK(!ui.key(press(VK_DOWN), host.state));
}

TEST_CASE(filterDialogIsModal) {
    RecordingHost host;
    ui::EditorUi ui(host);
    ui.resize(1440, 900);
    const auto openFilter = [&](int item) {
        ui.key(press(VK_F10), host.state);
        for (int i = 0; i < 5; ++i) ui.key(press(VK_RIGHT), host.state);  // «Фильтр»
        for (int i = 0; i < item; ++i) ui.key(press(VK_DOWN), host.state);
        ui.key(press(VK_RETURN), host.state);
    };
    openFilter(0);  // Gaussian
    CHECK(ui.overlayOpen() && host.fxChanges == 1 && host.lastFx.kind == core::FxKind::Gaussian && host.lastFx.radius == 6);
    // The dialog is 400 wide, centred: x = 520; the radius slider is its first row at y = top + 48.
    const double top = std::round((900 - (44 + 2 * 44 + 56)) / 2.0);
    const ui::SliderGeometry g = ui::sliderGeometry({536, top + 48, 368, 32});
    ui.pointer(mouse(platform::PointerPhase::Down, g.x1, top + 64), host.state);
    ui.pointer(mouse(platform::PointerPhase::Up, g.x1, top + 64, false), host.state);
    CHECK(host.lastFx.radius == 32 && host.fxChanges >= 2);
    // §11.2 Modal: a press outside is swallowed and the dialog stays; arrows change the focused value.
    CHECK(ui.pointer(mouse(platform::PointerPhase::Down, 100, 500), host.state));
    ui.pointer(mouse(platform::PointerPhase::Up, 100, 500, false), host.state);
    CHECK(ui.overlayOpen());
    ui.key(press(VK_LEFT), host.state);
    CHECK(host.lastFx.radius == 31);
    CHECK(ui.key(press('B'), host.state) && host.state.tool == ui::Tool::Brush);  // keys stay in the dialog
    ui.key(press(VK_ESCAPE), host.state);
    CHECK(!ui.overlayOpen() && host.fxCancelled == 1 && host.fxApplied == 0);
    openFilter(4);  // line extraction, then Enter applies
    CHECK(host.lastFx.kind == core::FxKind::AnimeLines && host.lastFx.toNewLayer);
    ui.key(press(VK_RETURN), host.state);
    CHECK(!ui.overlayOpen() && host.fxApplied == 1);
}

}  // namespace

int main() { return test::runAll(); }

#pragma once
// ChromaStudio Engine · ui · window layout in client DIP (spec §11.1). Pure geometry, no Win32/GL.
//
// Standard view (W ≥ 1000, H ≥ 600), R = clamp(round(2W/9), 264, 400) unless the user dragged the
// splitter (same limits):
//   menu (0,0,W,32) · tool options (0,32,W,48) · toolbar (0,80,56,H−104) · canvas (56,80,W−R−60,H−104)
//   splitter (W−R−4,80,4,H−104) · dock (W−R,80,R,H−104) · status (0,H−24,W,24)
// Tool buttons 40×40 at x = 8, y = 88 + 44i, scrolled when the toolbar is too short.
// Options: profile (8,40,144,32) · colour (160,40,36,32) · size (208,40,240,32) · opacity (464,40,224,32)
// · stabiliser (704,40,176,32). Below 1000 DIP the stabiliser moves to the "more" menu and the dock is
// hidden by default; below 760 DIP the compact row uses x = 8,120,160,352, widths 104,32,184,184, and
// the "more" button sits at x = 544. A hidden dock gives the canvas (56,80,W−56,H−104). Tab hides the
// panels (toolbar, options, dock) until pressed again.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace chroma::ui {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    [[nodiscard]] double right() const noexcept { return x + w; }
    [[nodiscard]] double bottom() const noexcept { return y + h; }
    [[nodiscard]] bool empty() const noexcept { return w <= 0.0 || h <= 0.0; }
    [[nodiscard]] bool contains(double px, double py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    [[nodiscard]] Rect inset(double d) const noexcept { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    [[nodiscard]] Rect intersect(const Rect& o) const noexcept {
        const double x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const double x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
    }
    friend bool operator==(const Rect&, const Rect&) = default;
};

/// Physical pixel edges of a DIP rectangle (§11): each edge is rounded on its own, so neighbouring
/// regions share a boundary.
struct EdgesPx {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

[[nodiscard]] inline EdgesPx physicalEdges(const Rect& r, double scale) noexcept {
    const auto edge = [scale](double v) { return static_cast<int>(std::lround(v * scale)); };
    return {edge(r.x), edge(r.y), edge(r.right()), edge(r.bottom())};
}

inline constexpr int kToolCount = 11;
inline constexpr double kMinDockWidth = 264.0;
inline constexpr double kMaxDockWidth = 400.0;

struct LayoutInput {
    double width = 1440.0;
    double height = 900.0;
    bool dockShown = true;      // the F7 state; see defaultDockShown
    bool panelsHidden = false;  // Tab
    double dockWidth = 0.0;     // user-dragged width; 0 = R from the formula
    double toolScroll = 0.0;    // DIP scrolled in the toolbar
};

struct ShellLayout {
    Rect menu, options, toolbar, canvas, splitter, dock, status;
    bool dockVisible = false;
    bool optionsVisible = true;
    bool toolbarVisible = true;
    bool compact = false;    // W < 760
    bool narrow = false;     // W < 1000
    bool shortDock = false;  // H < 600: dock content scrolls as tabs
    // Tool options row (empty rectangles are absent).
    Rect profile, color, size, opacity, stabilizer, more;
    // Toolbar buttons, already scrolled; toolScroll clamped to [0, maxToolScroll].
    std::array<Rect, kToolCount> tools{};
    double toolScroll = 0.0;
    double maxToolScroll = 0.0;
    // Dock interior: tabs 36, actions 36, properties 88 at the bottom, the list between.
    Rect dockTabs, dockActions, dockList, dockProperties;
};

/// The dock is shown by default only in the standard width.
inline bool defaultDockShown(double width) { return width >= 1000.0; }

inline double defaultDockWidth(double width) {
    return std::clamp(std::round(2.0 * width / 9.0), kMinDockWidth, kMaxDockWidth);
}

ShellLayout computeShellLayout(const LayoutInput& input);

// ── Sliders (spec §11.1 "Быстрые регуляторы") ────────────────────────────────

struct SliderGeometry {
    double x0 = 0.0;  // track from left + 8
    double x1 = 0.0;  // to right − 68
    Rect track;       // the bar
    Rect value;       // the editable value: the rightmost 60 DIP
};

SliderGeometry sliderGeometry(const Rect& control);
inline double sliderT(const SliderGeometry& g, double x) {
    return g.x1 > g.x0 ? std::clamp((x - g.x0) / (g.x1 - g.x0), 0.0, 1.0) : 0.0;
}
/// Diameter 0.25·8192ᵗ px over t ∈ [0, 1] (0.25 … 2048 px).
inline double diameterFromT(double t) { return 0.25 * std::pow(8192.0, std::clamp(t, 0.0, 1.0)); }
inline double tFromDiameter(double d) {
    return std::clamp(std::log(std::clamp(d, 0.25, 2048.0) / 0.25) / std::log(8192.0), 0.0, 1.0);
}

// ── Popups ───────────────────────────────────────────────────────────────────

/// Under the anchor if it fits, else above it; then x ∈ [8, W−pw−8], y ∈ [8, H−ph−8].
Rect placePopup(const Rect& anchor, double popupWidth, double popupHeight, double width, double height);

/// Brush catalogue: pw = min(456, W−16), ph = min(520, H−96).
inline Rect catalogSize(double width, double height) {
    return {0, 0, std::min(456.0, width - 16.0), std::min(520.0, height - 96.0)};
}

struct CatalogLayout {
    Rect search;      // 32 high
    Rect preview;     // 96 high
    Rect categories;  // 112 wide, on the left
    Rect list;        // profile rows of 40
};
/// Interior of the catalogue popup: padding 12.
CatalogLayout catalogLayout(const Rect& popup);

/// HSV popup: 248 × 280; the ring is centred at (124, 112) with radii 96 and 78, the S/V square is
/// (74, 62, 100, 100), all relative to the popup origin.
inline constexpr double kColorPopupWidth = 248.0;
inline constexpr double kColorPopupHeight = 280.0;
inline constexpr double kRingCenterX = 124.0;
inline constexpr double kRingCenterY = 112.0;
inline constexpr double kRingOuter = 96.0;
inline constexpr double kRingInner = 78.0;
inline constexpr Rect kSvSquare{74.0, 62.0, 100.0, 100.0};

// ── Brush preview (spec §11.1 "Каталог кистей") ──────────────────────────────

struct PreviewNode {
    double timeMs;
    double x, y;
    double pressure;
};
/// x(t) = 12 + (w − 24)t, y(t) = h/2 + 0.22h·sin(2πt), p(t) = 0.15 + 0.85·sin(πt), t ∈ [0, 1], 65 nodes
/// at a fixed interval.
std::vector<PreviewNode> previewCurve(double width, double height);

}  // namespace chroma::ui

#include "ui/Layout.hpp"

#include <numbers>

namespace chroma::ui {

ShellLayout computeShellLayout(const LayoutInput& in) {
    const double w = in.width;
    const double h = in.height;
    ShellLayout l;
    l.narrow = w < 1000.0;
    l.compact = w < 760.0;
    l.shortDock = h < 600.0;
    l.menu = {0, 0, w, 32};
    l.status = {0, h - 24, w, 24};

    if (in.panelsHidden) {
        // Tab: only the menu and the status bar stay.
        l.optionsVisible = false;
        l.toolbarVisible = false;
        l.dockVisible = false;
        l.canvas = {0, 32, w, h - 56};
        return l;
    }

    l.options = {0, 32, w, 48};
    l.toolbar = {0, 80, 56, h - 104};

    // Dock, if shown and if it leaves the canvas a usable width.
    const double r = in.dockWidth > 0 ? std::clamp(in.dockWidth, kMinDockWidth, kMaxDockWidth) : defaultDockWidth(w);
    l.dockVisible = in.dockShown && w - r - 60 >= 200.0;
    if (l.dockVisible) {
        l.canvas = {56, 80, w - r - 60, h - 104};
        l.splitter = {w - r - 4, 80, 4, h - 104};
        l.dock = {w - r, 80, r, h - 104};
        l.dockTabs = {l.dock.x, l.dock.y, l.dock.w, 36};
        l.dockActions = {l.dock.x, l.dock.y + 36, l.dock.w, 36};
        l.dockProperties = {l.dock.x, l.dock.bottom() - 88, l.dock.w, 88};
        l.dockList = {l.dock.x, l.dock.y + 72, l.dock.w, std::max(0.0, l.dock.h - 72 - 88)};
    } else {
        l.canvas = {56, 80, w - 56, h - 104};
    }

    // Tool options row.
    if (l.compact) {
        l.profile = {8, 40, 104, 32};
        l.color = {120, 40, 32, 32};
        l.size = {160, 40, 184, 32};
        l.opacity = {352, 40, 184, 32};
        l.more = {544, 40, 32, 32};
    } else {
        l.profile = {8, 40, 144, 32};
        l.color = {160, 40, 36, 32};
        l.size = {208, 40, 240, 32};
        l.opacity = {464, 40, 224, 32};
        if (l.narrow) {
            l.more = {704, 40, 32, 32};  // the stabiliser lives in this menu
        } else {
            l.stabilizer = {704, 40, 176, 32};
        }
    }

    // Toolbar: 11 buttons of 40 at y = 88 + 44i, scrolled into the visible part.
    const double content = 8 + 44.0 * kToolCount;
    l.maxToolScroll = std::max(0.0, content - l.toolbar.h);
    l.toolScroll = std::clamp(in.toolScroll, 0.0, l.maxToolScroll);
    for (int i = 0; i < kToolCount; ++i) l.tools[static_cast<std::size_t>(i)] = {8, 88 + 44.0 * i - l.toolScroll, 40, 40};
    return l;
}

SliderGeometry sliderGeometry(const Rect& control) {
    SliderGeometry g;
    g.x0 = control.x + 8;
    g.x1 = control.right() - 68;
    g.track = {g.x0, control.y + control.h / 2 - 2, std::max(0.0, g.x1 - g.x0), 4};
    g.value = {control.right() - 60, control.y + 2, 60, control.h - 4};
    return g;
}

Rect placePopup(const Rect& anchor, double pw, double ph, double width, double height) {
    Rect r{anchor.x, anchor.bottom() + 4, pw, ph};
    if (r.bottom() > height - 8) r.y = anchor.y - ph - 4;  // above the anchor
    r.x = std::clamp(r.x, 8.0, std::max(8.0, width - pw - 8));
    r.y = std::clamp(r.y, 8.0, std::max(8.0, height - ph - 8));
    return r;
}

CatalogLayout catalogLayout(const Rect& popup) {
    const Rect inner = popup.inset(12);
    CatalogLayout c;
    c.search = {inner.x, inner.y, inner.w, 32};
    c.preview = {inner.x, inner.y + 40, inner.w, 96};
    const double top = inner.y + 144;
    const double rest = std::max(0.0, inner.bottom() - top);
    c.categories = {inner.x, top, 112, rest};
    c.list = {inner.x + 120, top, std::max(0.0, inner.w - 120), rest};
    return c;
}

std::vector<PreviewNode> previewCurve(double width, double height) {
    constexpr int kNodes = 65;
    constexpr double kStepMs = 8.0;  // fixed time between nodes
    std::vector<PreviewNode> nodes;
    nodes.reserve(kNodes);
    for (int i = 0; i < kNodes; ++i) {
        const double t = static_cast<double>(i) / (kNodes - 1);
        nodes.push_back({i * kStepMs, 12 + (width - 24) * t,
                         height / 2 + 0.22 * height * std::sin(2 * std::numbers::pi * t),
                         0.15 + 0.85 * std::sin(std::numbers::pi * t)});
    }
    return nodes;
}

}  // namespace chroma::ui

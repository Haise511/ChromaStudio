#include "ui/BrushPreview.hpp"

#include "core/BrushProfile.hpp"
#include "core/Half.hpp"
#include "core/StrokeBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace chroma::ui {

BrushPreview::BrushPreview(const gl::Context& context) : tiles_(context, 64, 64) {}

double BrushPreview::curveDiameter(double profileDiameter, double height) {
    return std::clamp(std::min(profileDiameter, 0.42 * height), 1.0, 2048.0);
}

void BrushPreview::draw(UiRenderer& r, const core::BrushProfile& profile, const Rect& area, const core::Rgb& color,
                        double alpha) {
    // Each edge rounded on its own, as the shell does (§11).
    const double scale = r.scale();
    const int x0 = static_cast<int>(std::lround(area.x * scale));
    const int y0 = static_cast<int>(std::lround(area.y * scale));
    const int x1 = static_cast<int>(std::lround(area.right() * scale));
    const int y1 = static_cast<int>(std::lround(area.bottom() * scale));
    if (x1 - x0 < 16 || y1 - y0 < 16) {
        r.restoreState();
        return;
    }
    Entry& entry = entryFor({profile, x1 - x0, y1 - y0, color, alpha});
    tiles_.setDocumentSize(entry.key.width, entry.key.height);  // cached entries may differ in size

    render::DocumentFrame frame;
    frame.clientWidth = r.clientWidth();
    frame.clientHeight = r.clientHeight();
    frame.viewportX0 = std::max(x0, 0);
    frame.viewportY0 = std::max(y0, 0);
    frame.viewportX1 = std::min(x1, frame.clientWidth);
    frame.viewportY1 = std::min(y1, frame.clientHeight);
    frame.originX = x0;
    frame.originY = y0;
    frame.pixelsPerDocPx = 1.0;
    if (frame.viewportX1 > frame.viewportX0 && frame.viewportY1 > frame.viewportY0) {
        static const core::LayerTree kOneLayer = core::LayerTree::single();
        render::LayerStack stack;
        stack.tree = &kOneLayer;
        stack.tiles = [&entry](std::uint64_t, int tx, int ty) -> render::TileRef {
            const auto it = entry.tiles.find({tx, ty});
            return it == entry.tiles.end() ? nullptr : it->second;
        };
        tiles_.drawDocument(frame, stack);
    }
    r.restoreState();
}

BrushPreview::Entry& BrushPreview::entryFor(const Key& key) {
    ++clock_;
    for (Entry& e : cache_) {
        if (e.key == key) {
            e.lastUse = clock_;
            return e;
        }
    }
    if (cache_.size() >= kCacheSize) {
        // Least recently used goes; its versions release their GPU copies with it.
        cache_.erase(std::min_element(cache_.begin(), cache_.end(),
                                      [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; }));
    }
    Entry& e = cache_.emplace_back();
    e.key = key;
    e.lastUse = clock_;
    render(e);
    return e;
}

void BrushPreview::render(Entry& entry) {
    const Key& key = entry.key;
    const core::BrushProfile& profile = key.profile;
    tiles_.setDocumentSize(key.width, key.height);
    const render::TileLookup lookup = [&entry](int tx, int ty) -> render::TileRef {
        const auto it = entry.tiles.find({tx, ty});
        return it == entry.tiles.end() ? nullptr : it->second;
    };
    render::BrushSetup setup = render::brushSetupFor(
        profile, core::premultipliedInk(key.color[0], key.color[1], key.color[2], key.alpha), false);
    if (resolver_) resolver_(setup, profile);

    const auto stroke = [&](const std::vector<core::StrokeSample>& samples, double diameter) {
        core::StrokeBuilder builder(profile, diameter);
        std::vector<core::Dab> dabs;
        tiles_.beginStroke(setup, lookup);
        for (std::size_t i = 0; i + 1 < samples.size(); ++i) builder.add(samples[i], dabs);
        core::StrokeResult result = builder.finish(samples.back());
        if (result.replay) {
            dabs = std::move(result.dabs);
        } else {
            dabs.insert(dabs.end(), result.dabs.begin(), result.dabs.end());
        }
        tiles_.applyDabs(dabs);
        for (const render::TileChange& c : tiles_.commitStroke()) {
            if (c.after) {
                entry.tiles[{c.key.tx, c.key.ty}] = c.after;
            } else {
                entry.tiles.erase({c.key.tx, c.key.ty});
            }
        }
    };

    if (profile.wet) {
        // §11.1: the watercolor preview gets a constant two-colour ground to carry pigment from.
        const std::array<float, 4> left = core::premultipliedInk(0.98, 0.92, 0.74, 1.0);
        const std::array<float, 4> right = core::premultipliedInk(0.72, 0.86, 0.97, 1.0);
        for (int ty = 0; ty * render::kTileSize < key.height; ++ty) {
            for (int tx = 0; tx * render::kTileSize < key.width; ++tx) {
                auto data = std::make_shared<core::TileData>();
                const core::TileExtent extent = core::tileExtent(key.width, key.height, tx, ty);
                for (int y = 0; y < extent.validHeight; ++y) {
                    for (int x = 0; x < extent.validWidth; ++x) {
                        const bool isLeft = tx * render::kTileSize + x < key.width / 2;
                        const auto& c = isLeft ? left : right;
                        const std::size_t i = (static_cast<std::size_t>(y) * render::kTileSize + static_cast<std::size_t>(x)) * 4;
                        for (std::size_t k = 0; k < 4; ++k) data->halfs[i + k] = core::floatToHalf(c[k]);
                    }
                }
                entry.tiles[{tx, ty}] = render::TileRenderer::makeVersion(std::move(data));
            }
        }
    }
    const double h = key.height;
    // The natural tip shape at full pressure, centred in the left square.
    stroke({core::StrokeSample{0.0, {h / 2, h / 2}, 1.0}}, std::clamp(std::min(profile.tip.diameter, 0.8 * h), 1.0, 2048.0));
    // The §11.1 test curve in the rest.
    std::vector<core::StrokeSample> samples;
    for (const PreviewNode& n : previewCurve(key.width - h, h)) samples.push_back({n.timeMs, {n.x + h, n.y}, n.pressure});
    stroke(samples, curveDiameter(profile.tip.diameter, h));
}

}  // namespace chroma::ui

#pragma once
// ChromaStudio Engine · ui · brush previews drawn by the real engine (spec §11.1 "Каталог кистей").
//
// One preview is a natural stamp at full pressure in the left h × h square, then the §11.1 test curve
// x(t) = 12 + (w − 24)t, y(t) = h/2 + 0.22h·sin 2πt, p(t) = 0.15 + 0.85·sin πt with fixed times in the
// rest. It is stroked once per (brush, size in px, colour) by a TileRenderer of its own, kept as RAM
// tiles in a small LRU cache and drawn like a tiny document at 1 document px per physical px. Watercolor
// is drawn over a constant two-colour ground, since it carries pigment from the surface (§5).

#include "core/Color.hpp"
#include "render_gl/TileRenderer.hpp"
#include "ui/Layout.hpp"
#include "ui/UiRenderer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace chroma::ui {

class BrushPreview {
public:
    explicit BrushPreview(const gl::Context& context);

    /// Draws `profile` into `area` (DIP) of the current frame, over whatever is already there (the
    /// caller paints the paper), then restores the chrome's GL state.
    void draw(UiRenderer& r, const core::BrushProfile& profile, const Rect& area, const core::Rgb& color, double alpha);
    /// Attaches the masks a profile refers to (§14.2).
    void setMaskResolver(std::function<void(render::BrushSetup&, const core::BrushProfile&)> resolver) {
        resolver_ = std::move(resolver);
    }

    /// Diameter used for the curve: the profile's own, at most 0.42·h so the curve fits.
    [[nodiscard]] static double curveDiameter(double profileDiameter, double height);

private:
    struct Key {
        core::BrushProfile profile;
        int width = 0;
        int height = 0;
        core::Rgb color{};
        double alpha = 1.0;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Entry {
        Key key;
        std::map<std::pair<int, int>, render::TileRef> tiles;
        std::uint64_t lastUse = 0;
    };

    Entry& entryFor(const Key& key);
    void render(Entry& entry);

    static constexpr std::size_t kCacheSize = 8;

    render::TileRenderer tiles_;
    std::function<void(render::BrushSetup&, const core::BrushProfile&)> resolver_;
    std::vector<Entry> cache_;
    std::uint64_t clock_ = 0;
};

}  // namespace chroma::ui

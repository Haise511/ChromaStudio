#pragma once
// ChromaStudio Engine · core · view transform (spec §2).
//
// u = pan + z·R(θ)·x maps a document point x (px) to viewport DIP u; x = R(−θ)·(u − pan)/z.
// z ∈ [1/64, 64] DIP per document px. Zooming around the cursor u₀ keeps x₀ under it:
// pan′ = u₀ − z′·R(θ′)·x₀. Rotation is not exposed yet (θ = 0).

#include "core/Geometry.hpp"

#include <algorithm>

namespace chroma::core {

struct ViewTransform {
    static constexpr double kMinZoom = 1.0 / 64.0;
    static constexpr double kMaxZoom = 64.0;

    double zoom = 1.0;  // DIP per document px
    V pan;              // DIP, relative to the viewport origin

    [[nodiscard]] V toView(V document) const { return pan + document * zoom; }
    [[nodiscard]] V toDocument(V view) const { return (view - pan) * (1.0 / zoom); }

    void zoomAround(V anchor, double newZoom) {
        const V fixed = toDocument(anchor);
        zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
        pan = anchor - fixed * zoom;
    }

    /// Centres the document in the viewport, scaled to `fraction` of the tighter axis.
    void fit(V viewport, V document, double fraction = 0.92) {
        if (document.x <= 0 || document.y <= 0 || viewport.x <= 0 || viewport.y <= 0) return;
        zoom = std::clamp(std::min(viewport.x / document.x, viewport.y / document.y) * fraction, kMinZoom, kMaxZoom);
        pan = (viewport - document * zoom) * 0.5;
    }

    /// 1 physical pixel per document pixel at the given DPI, centred on the viewport.
    void actualPixels(V viewport, V document, double dpi) {
        zoomAround(viewport * 0.5, 96.0 / dpi);
        pan = (viewport - document * zoom) * 0.5;
    }
};

}  // namespace chroma::core

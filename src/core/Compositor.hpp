#pragma once
// ChromaStudio Engine · core · the layer compositor on the CPU (spec §7, §7.1).
//
// The reference the GPU path is checked against (§12: GPU/CPU parity within 0.002 per operation),
// and the path for everything that needs the flattened image without a context: PNG export, the
// pipette, thumbnails. blendPixel is the §7.1 fragment program line by line in float; compose
// applies the tree rules of §7:
//   * children bottom to top; a group is first composed on transparent, then blended as one source
//     with its own opacity and mode;
//   * a clip chain (consecutive clipped siblings above a base) is composed on the base colour at unit
//     coverage, then multiplied by aBase·opacityBase and blended in the base's mode; a hidden or empty
//     base hides the chain, an orphan clip contributes nothing;
//   * every intermediate result is rounded to binary16, as the GPU's RGBA16F targets do.

#include "core/LayerTree.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace chroma::core {

/// Linear premultiplied RGBA.
struct Pixel {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    friend bool operator==(const Pixel&, const Pixel&) = default;
};

/// §7: `src` with `opacity` and blend mode `mode` over `dst`. Hardware blending plays no part.
Pixel blendPixel(const Pixel& dst, const Pixel& src, float opacity, int mode);

/// Rounds each channel to binary16 and back.
Pixel quantize(const Pixel& p);

/// Fills `out` with a raster layer's pixels for the region being composed (every call asks for the
/// same region); returns false when the layer is transparent there.
using LayerFetch = std::function<bool(std::uint64_t layer, std::span<Pixel> out)>;

/// Composes the visible tree into `out` (every pixel is written).
void compose(const LayerTree& tree, std::span<Pixel> out, const LayerFetch& fetch);

}  // namespace chroma::core

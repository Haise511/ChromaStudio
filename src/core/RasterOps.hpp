#pragma once
// ChromaStudio Engine · core · raster operations of the editor on the CPU (spec §10.1).
//
// Each operation reads canonical tiles and returns the tiles it changes as new canonical content
// (nullptr: the tile becomes transparent), ready to be published as one history command:
//   floodMask    magic wand: from a seed, pixels whose largest absolute PMA difference to the seed is
//                within the tolerance; horizontal spans and four neighbours when connected, the whole
//                canvas otherwise.
//   fillRegion   source-over of the ink weighted by region ∩ selection; Alpha Lock keeps a0 (§7).
//   moveLayer    integer move: with a mask the part m·P moves and (1 − m)·P stays, then the moved part
//                is composed over what stays; whole-pixel moves copy exactly, a zero move changes nothing.
//   clearMasked  Cut: (1 − m)·P.
//   extract / placeImage   the clipboard's straight sRGB RGBA8 image out of and into a layer.

#include "core/Compositor.hpp"
#include "core/Selection.hpp"
#include "core/TileData.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace chroma::core {

using TileContent = std::shared_ptr<const TileData>;
/// The layer's committed tile, or nullptr when it is transparent.
using TileSource = std::function<TileContent(int tx, int ty)>;
/// A pixel of whatever the wand looks at (the active layer or the composite), linear PMA.
using PixelSource = std::function<Pixel(int x, int y)>;

struct TileEdit {
    int tx = 0;
    int ty = 0;
    TileContent after;  // nullptr: transparent
};

/// Reads pixels of a tile source through a small cache.
class TileReader {
public:
    explicit TileReader(TileSource source) : source_(std::move(source)) {}
    [[nodiscard]] Pixel at(int x, int y);
    [[nodiscard]] const TileContent& tile(int tx, int ty);

private:
    TileSource source_;
    std::map<std::pair<int, int>, TileContent> cache_;
};

SelectionMask floodMask(const PixelSource& at, int width, int height, int x, int y, double tolerance, bool connected);

std::vector<TileEdit> fillRegion(const TileSource& layer, const SelectionMask& region,
                                 const std::optional<SelectionMask>& selection, const Pixel& ink, bool alphaLock,
                                 int width, int height);

/// `present` lists the tiles the layer has (others are transparent). Output is clipped to width × height,
/// which for a crop is the new canvas (the source is read in its own coordinates).
std::vector<TileEdit> moveLayer(const TileSource& layer, const std::vector<std::pair<int, int>>& present,
                                const std::optional<SelectionMask>& mask, int dx, int dy, int width, int height);

std::vector<TileEdit> clearMasked(const TileSource& layer, const SelectionMask& mask, int width, int height);

/// The layer split by a mask for a move preview: base = (1 − m)·P, floating = m·P. Tiles the mask does
/// not touch stay in base as they are.
struct MaskedSplit {
    std::map<std::pair<int, int>, TileContent> base;
    std::map<std::pair<int, int>, TileContent> floating;
};
MaskedSplit splitMasked(const TileSource& layer, const std::vector<std::pair<int, int>>& present, const SelectionMask& mask,
                        int width, int height);

/// Straight sRGB RGBA8, rows top to bottom, placed at (x, y) in the document.
struct Image8 {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

/// The layer's pixels times the mask, cropped to the mask's bounds (the layer's content without one).
Image8 extract(const TileSource& layer, const std::vector<std::pair<int, int>>& present,
               const std::optional<SelectionMask>& mask, int width, int height);

/// An image as tiles of a new layer, clipped to the document.
std::vector<TileEdit> placeImage(const Image8& image, int width, int height);

}  // namespace chroma::core

#pragma once
// ChromaStudio Engine · core · the selection mask (spec §10.1, §10.3).
//
// A sparse R8 mask of the document's size in 128-px tiles of 16 384 bytes; a missing tile is zero.
// "No selection" is represented by its absence (std::optional) and allows writing everywhere; an
// existing empty selection forbids writing everywhere. The lasso closes on release and covers pixels
// by the even–odd rule on a 4 × 4 grid of samples per pixel. For masks a, b the operations are
// replace = b, add = max(a, b), intersect = min(a, b), subtract = a·(1 − b).

#include "core/Geometry.hpp"
#include "core/TileData.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chroma::core {

inline constexpr std::size_t kMaskTileBytes = static_cast<std::size_t>(kTileSize) * kTileSize;  // 16 384

struct MaskTile {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(kMaskTileBytes, 0);
    friend bool operator==(const MaskTile&, const MaskTile&) = default;
};
using MaskRef = std::shared_ptr<const MaskTile>;

/// Half-open document pixel box.
struct PixelBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] int width() const noexcept { return x1 - x0; }
    [[nodiscard]] int height() const noexcept { return y1 - y0; }
    friend bool operator==(const PixelBox&, const PixelBox&) = default;
};

class SelectionMask {
public:
    using Key = std::pair<int, int>;  // (tx, ty)

    [[nodiscard]] std::uint8_t at(int x, int y) const;
    [[nodiscard]] MaskRef tile(int tx, int ty) const;
    void setTile(int tx, int ty, MaskRef tile);  // an all-zero tile is dropped
    [[nodiscard]] const std::map<Key, MaskRef>& tiles() const noexcept { return tiles_; }
    /// No pixel selected.
    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }
    /// Bounding box of the nonzero pixels; empty when nothing is selected.
    [[nodiscard]] PixelBox bounds() const;

    friend bool operator==(const SelectionMask&, const SelectionMask&) = default;

private:
    std::map<Key, MaskRef> tiles_;
};

enum class SelectOp : std::uint8_t { Replace, Add, Subtract, Intersect };

/// §10.1: the polygon closed, even–odd, coverage of the 16 samples (x + (i + ½)/4, y + (j + ½)/4).
SelectionMask rasterizeLasso(const std::vector<V>& polygon, int width, int height);
/// Every pixel of the document.
SelectionMask selectAll(int width, int height);
/// 255 − a inside the document.
SelectionMask invert(const SelectionMask& mask, int width, int height);
/// Combines the current selection (nullopt: none, which every operation treats as replace) with b.
SelectionMask combine(const std::optional<SelectionMask>& current, const SelectionMask& b, SelectOp op);
/// Moves the mask by whole pixels; what leaves [0, width) × [0, height) is dropped.
SelectionMask translate(const SelectionMask& mask, int dx, int dy, int width, int height);

}  // namespace chroma::core

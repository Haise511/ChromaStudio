#pragma once
// ChromaStudio Engine · core · canonical tile content (spec §2, §10.3).
//
// 128 rows of 128 pixels, top row first, RGBA binary16, linear premultiplied: 0 ≤ rgb ≤ a ≤ 1.
// Alpha zero canonicalises RGB to zero, and pixels outside the document (the invisible remainder of
// an edge tile) are zero. In memory the halfs are in native order, which is little-endian on x64,
// so the buffer is byte-for-byte the 131 072-byte tile record of the project file.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chroma::core {

inline constexpr int kTileSize = 128;
inline constexpr std::size_t kTileHalfs = static_cast<std::size_t>(kTileSize) * kTileSize * 4;
inline constexpr std::size_t kTileBytes = kTileHalfs * sizeof(std::uint16_t);  // 131 072

static_assert(std::endian::native == std::endian::little, "tile records are little-endian binary16");

struct TileData {
    std::vector<std::uint16_t> halfs = std::vector<std::uint16_t>(kTileHalfs, 0);

    friend bool operator==(const TileData&, const TileData&) = default;
};

/// Extent of the document inside tile (tx, ty): columns [0, validWidth), rows [0, validHeight).
struct TileExtent {
    int validWidth = kTileSize;
    int validHeight = kTileSize;
};

TileExtent tileExtent(int documentWidth, int documentHeight, int tx, int ty) noexcept;

/// Brings GPU output to canonical form: NaN → 0, a ∈ [0, 1], rgb ∈ [0, a], a = 0 → rgb = 0,
/// −0 → +0, zero outside the extent. Returns true when any half changed.
bool canonicalize(TileData& tile, TileExtent extent);

/// Checks a tile read from a file (§10.3): no NaN/Inf, PMA bounds, zero outside the extent.
/// Returns an empty string when valid, otherwise the first problem.
std::string validateTile(const TileData& tile, TileExtent extent);

/// True when every half is +0 (the tile is then represented by its absence).
bool isTransparent(const TileData& tile) noexcept;

}  // namespace chroma::core

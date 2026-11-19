#include "core/TileData.hpp"

#include "core/Half.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace chroma::core {

TileExtent tileExtent(int documentWidth, int documentHeight, int tx, int ty) noexcept {
    return {std::clamp(documentWidth - tx * kTileSize, 0, kTileSize),
            std::clamp(documentHeight - ty * kTileSize, 0, kTileSize)};
}

bool canonicalize(TileData& tile, TileExtent extent) {
    bool changed = false;
    const auto store = [&](std::uint16_t& slot, float value) {
        const std::uint16_t half = floatToHalf(value);
        if (half != slot) {
            slot = half;
            changed = true;
        }
    };
    for (int y = 0; y < kTileSize; ++y) {
        for (int x = 0; x < kTileSize; ++x) {
            std::uint16_t* p = &tile.halfs[(static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4];
            if (x >= extent.validWidth || y >= extent.validHeight) {
                for (int c = 0; c < 4; ++c) store(p[c], 0.0f);
                continue;
            }
            float a = halfToFloat(p[3]);
            a = std::isnan(a) ? 0.0f : std::clamp(a, 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                const float v = halfToFloat(p[c]);
                // + 0.0f turns −0 into +0.
                store(p[c], a == 0.0f || std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, a) + 0.0f);
            }
            store(p[3], a + 0.0f);
        }
    }
    return changed;
}

std::string validateTile(const TileData& tile, TileExtent extent) {
    if (tile.halfs.size() != kTileHalfs) return "tile size is not 131072 bytes";
    for (int y = 0; y < kTileSize; ++y) {
        for (int x = 0; x < kTileSize; ++x) {
            const std::uint16_t* p =
                &tile.halfs[(static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4];
            if (x >= extent.validWidth || y >= extent.validHeight) {
                if (p[0] | p[1] | p[2] | p[3]) return std::format("non-zero padding at ({}, {})", x, y);
                continue;
            }
            const float a = halfToFloat(p[3]);
            if (!std::isfinite(a) || a < 0.0f || a > 1.0f) return std::format("alpha out of range at ({}, {})", x, y);
            for (int c = 0; c < 3; ++c) {
                const float v = halfToFloat(p[c]);
                if (!std::isfinite(v) || v < 0.0f || v > a) {
                    return std::format("colour outside 0 <= rgb <= a at ({}, {})", x, y);
                }
            }
        }
    }
    return {};
}

bool isTransparent(const TileData& tile) noexcept {
    return std::all_of(tile.halfs.begin(), tile.halfs.end(), [](std::uint16_t h) { return h == 0; });
}

}  // namespace chroma::core

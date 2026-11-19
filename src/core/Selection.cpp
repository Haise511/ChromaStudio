#include "core/Selection.hpp"

#include <algorithm>
#include <cmath>

namespace chroma::core {
namespace {

std::shared_ptr<MaskTile> blank() { return std::make_shared<MaskTile>(); }

bool allZero(const MaskTile& t) {
    return std::all_of(t.bytes.begin(), t.bytes.end(), [](std::uint8_t b) { return b == 0; });
}

int tilesFor(int pixels) { return (pixels + kTileSize - 1) / kTileSize; }

}  // namespace

std::uint8_t SelectionMask::at(int x, int y) const {
    if (x < 0 || y < 0) return 0;
    const MaskRef t = tile(x / kTileSize, y / kTileSize);
    return t ? t->bytes[static_cast<std::size_t>(y % kTileSize) * kTileSize + static_cast<std::size_t>(x % kTileSize)] : 0;
}

MaskRef SelectionMask::tile(int tx, int ty) const {
    const auto it = tiles_.find({tx, ty});
    return it == tiles_.end() ? nullptr : it->second;
}

void SelectionMask::setTile(int tx, int ty, MaskRef tile) {
    if (!tile || allZero(*tile)) {
        tiles_.erase({tx, ty});
    } else {
        tiles_[{tx, ty}] = std::move(tile);
    }
}

PixelBox SelectionMask::bounds() const {
    PixelBox box{0, 0, 0, 0};
    bool any = false;
    for (const auto& [key, t] : tiles_) {
        for (int y = 0; y < kTileSize; ++y) {
            for (int x = 0; x < kTileSize; ++x) {
                if (t->bytes[static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)] == 0) continue;
                const int dx = key.first * kTileSize + x;
                const int dy = key.second * kTileSize + y;
                if (!any) {
                    box = {dx, dy, dx + 1, dy + 1};
                    any = true;
                } else {
                    box = {std::min(box.x0, dx), std::min(box.y0, dy), std::max(box.x1, dx + 1), std::max(box.y1, dy + 1)};
                }
            }
        }
    }
    return box;
}

SelectionMask rasterizeLasso(const std::vector<V>& polygon, int width, int height) {
    SelectionMask mask;
    if (polygon.size() < 3 || width <= 0 || height <= 0) return mask;
    double minY = polygon[0].y, maxY = polygon[0].y, minX = polygon[0].x, maxX = polygon[0].x;
    for (const V& p : polygon) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }
    const int y0 = std::clamp(static_cast<int>(std::floor(minY)), 0, height);
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)) + 1, 0, height);
    const int x0 = std::clamp(static_cast<int>(std::floor(minX)), 0, width);
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxX)) + 1, 0, width);
    if (y0 >= y1 || x0 >= x1) return mask;

    std::map<SelectionMask::Key, std::shared_ptr<MaskTile>> built;
    std::vector<int> counts(static_cast<std::size_t>(x1 - x0));
    std::vector<double> crossings;
    for (int y = y0; y < y1; ++y) {
        std::fill(counts.begin(), counts.end(), 0);
        for (int j = 0; j < 4; ++j) {
            const double sy = y + (j + 0.5) / 4.0;
            crossings.clear();
            for (std::size_t e = 0; e < polygon.size(); ++e) {
                const V a = polygon[e];
                const V b = polygon[(e + 1) % polygon.size()];  // closed on release
                if ((a.y <= sy) == (b.y <= sy)) continue;    // half-open rule: no double counting
                crossings.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t k = 0; k + 1 < crossings.size(); k += 2) {  // even–odd
                // Sample columns x + (i + ½)/4 inside [c0, c1).
                const double c0 = crossings[k];
                const double c1 = crossings[k + 1];
                const int s0 = std::max(static_cast<int>(std::ceil(c0 * 4.0 - 0.5)), x0 * 4);
                const int s1 = std::min(static_cast<int>(std::ceil(c1 * 4.0 - 0.5)), x1 * 4);
                for (int s = s0; s < s1; ++s) ++counts[static_cast<std::size_t>(s / 4 - x0)];
            }
        }
        for (int x = x0; x < x1; ++x) {
            const int c = counts[static_cast<std::size_t>(x - x0)];
            if (c == 0) continue;
            auto& t = built[{x / kTileSize, y / kTileSize}];
            if (!t) t = blank();
            t->bytes[static_cast<std::size_t>(y % kTileSize) * kTileSize + static_cast<std::size_t>(x % kTileSize)] =
                static_cast<std::uint8_t>((c * 255 + 8) / 16);
        }
    }
    for (auto& [key, t] : built) mask.setTile(key.first, key.second, std::move(t));
    return mask;
}

SelectionMask selectAll(int width, int height) {
    SelectionMask mask;
    for (int ty = 0; ty < tilesFor(height); ++ty) {
        for (int tx = 0; tx < tilesFor(width); ++tx) {
            auto t = blank();
            const TileExtent e = tileExtent(width, height, tx, ty);
            for (int y = 0; y < e.validHeight; ++y) {
                std::fill_n(t->bytes.begin() + static_cast<std::ptrdiff_t>(y * kTileSize), e.validWidth, std::uint8_t{255});
            }
            mask.setTile(tx, ty, std::move(t));
        }
    }
    return mask;
}

SelectionMask invert(const SelectionMask& mask, int width, int height) {
    SelectionMask out;
    for (int ty = 0; ty < tilesFor(height); ++ty) {
        for (int tx = 0; tx < tilesFor(width); ++tx) {
            auto t = blank();
            const MaskRef src = mask.tile(tx, ty);
            const TileExtent e = tileExtent(width, height, tx, ty);
            for (int y = 0; y < e.validHeight; ++y) {
                for (int x = 0; x < e.validWidth; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x);
                    t->bytes[i] = static_cast<std::uint8_t>(255 - (src ? src->bytes[i] : 0));
                }
            }
            out.setTile(tx, ty, std::move(t));
        }
    }
    return out;
}

SelectionMask combine(const std::optional<SelectionMask>& current, const SelectionMask& b, SelectOp op) {
    if (!current || op == SelectOp::Replace) return b;
    const SelectionMask& a = *current;
    SelectionMask out;
    std::map<SelectionMask::Key, bool> keys;
    for (const auto& [k, t] : a.tiles()) keys[k] = true;
    for (const auto& [k, t] : b.tiles()) keys[k] = true;
    for (const auto& [k, unused] : keys) {
        const MaskRef ta = a.tile(k.first, k.second);
        const MaskRef tb = b.tile(k.first, k.second);
        auto t = blank();
        for (std::size_t i = 0; i < kMaskTileBytes; ++i) {
            const int va = ta ? ta->bytes[i] : 0;
            const int vb = tb ? tb->bytes[i] : 0;
            int v = 0;
            switch (op) {
            case SelectOp::Add: v = std::max(va, vb); break;
            case SelectOp::Intersect: v = std::min(va, vb); break;
            case SelectOp::Subtract: v = (va * (255 - vb) + 127) / 255; break;
            case SelectOp::Replace: v = vb; break;
            }
            t->bytes[i] = static_cast<std::uint8_t>(v);
        }
        out.setTile(k.first, k.second, std::move(t));
    }
    return out;
}

SelectionMask translate(const SelectionMask& mask, int dx, int dy, int width, int height) {
    if (dx == 0 && dy == 0) return mask;
    std::map<SelectionMask::Key, std::shared_ptr<MaskTile>> built;
    for (const auto& [k, t] : mask.tiles()) {
        for (int y = 0; y < kTileSize; ++y) {
            for (int x = 0; x < kTileSize; ++x) {
                const std::uint8_t v = t->bytes[static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)];
                if (v == 0) continue;
                const int nx = k.first * kTileSize + x + dx;
                const int ny = k.second * kTileSize + y + dy;
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                auto& out = built[{nx / kTileSize, ny / kTileSize}];
                if (!out) out = blank();
                out->bytes[static_cast<std::size_t>(ny % kTileSize) * kTileSize + static_cast<std::size_t>(nx % kTileSize)] = v;
            }
        }
    }
    SelectionMask out;
    for (auto& [k, t] : built) out.setTile(k.first, k.second, std::move(t));
    return out;
}

}  // namespace chroma::core

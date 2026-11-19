#include "core/RasterOps.hpp"

#include "core/Color.hpp"
#include "core/Half.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace chroma::core {
namespace {

int tilesFor(int pixels) { return (pixels + kTileSize - 1) / kTileSize; }

std::size_t pixelIndex(int x, int y) {
    return (static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)) * 4;
}

Pixel readPixel(const TileData& t, int x, int y) {
    const std::size_t i = pixelIndex(x, y);
    return {halfToFloat(t.halfs[i]), halfToFloat(t.halfs[i + 1]), halfToFloat(t.halfs[i + 2]), halfToFloat(t.halfs[i + 3])};
}

void writePixel(TileData& t, int x, int y, const Pixel& p) {
    const std::size_t i = pixelIndex(x, y);
    t.halfs[i] = floatToHalf(p.r);
    t.halfs[i + 1] = floatToHalf(p.g);
    t.halfs[i + 2] = floatToHalf(p.b);
    t.halfs[i + 3] = floatToHalf(p.a);
}

Pixel scaled(const Pixel& p, float k) { return {p.r * k, p.g * k, p.b * k, p.a * k}; }

Pixel over(const Pixel& top, const Pixel& below) {
    const float k = 1.0f - top.a;
    return {top.r + below.r * k, top.g + below.g * k, top.b + below.b * k, top.a + below.a * k};
}

float maskAt(const std::optional<SelectionMask>& mask, int x, int y) {
    return mask ? mask->at(x, y) / 255.0f : 1.0f;
}

/// Canonical form of a finished tile; nullptr when transparent.
TileContent finish(std::shared_ptr<TileData> data, int width, int height, int tx, int ty) {
    canonicalize(*data, tileExtent(width, height, tx, ty));
    if (isTransparent(*data)) return nullptr;
    return data;
}

/// Builds the given output tiles pixel by pixel from `fn(x, y)`; returns those whose content changed.
std::vector<TileEdit> rebuild(TileReader& before, const std::set<std::pair<int, int>>& keys, int width, int height,
                              const std::function<Pixel(int, int)>& fn) {
    std::vector<TileEdit> edits;
    for (const auto& [tx, ty] : keys) {
        if (tx < 0 || ty < 0 || tx >= tilesFor(width) || ty >= tilesFor(height)) continue;
        auto data = std::make_shared<TileData>();
        const TileExtent e = tileExtent(width, height, tx, ty);
        for (int y = 0; y < e.validHeight; ++y) {
            for (int x = 0; x < e.validWidth; ++x) writePixel(*data, x, y, fn(tx * kTileSize + x, ty * kTileSize + y));
        }
        TileContent after = finish(std::move(data), width, height, tx, ty);
        const TileContent& old = before.tile(tx, ty);
        const bool same = (!after && !old) || (after && old && *after == *old);
        if (!same) edits.push_back({tx, ty, std::move(after)});
    }
    return edits;
}

}  // namespace

const TileContent& TileReader::tile(int tx, int ty) {
    const auto key = std::pair{tx, ty};
    auto it = cache_.find(key);
    if (it == cache_.end()) it = cache_.emplace(key, tx < 0 || ty < 0 ? nullptr : source_(tx, ty)).first;
    return it->second;
}

Pixel TileReader::at(int x, int y) {
    if (x < 0 || y < 0) return {};
    const TileContent& t = tile(x / kTileSize, y / kTileSize);
    return t ? readPixel(*t, x % kTileSize, y % kTileSize) : Pixel{};
}

SelectionMask floodMask(const PixelSource& at, int width, int height, int x, int y, double tolerance, bool connected) {
    SelectionMask mask;
    if (x < 0 || y < 0 || x >= width || y >= height) return mask;
    const Pixel seed = at(x, y);
    const auto tol = static_cast<float>(std::clamp(tolerance, 0.0, 1.0));
    const auto matches = [&](int px, int py) {
        const Pixel p = at(px, py);
        return std::max({std::abs(p.r - seed.r), std::abs(p.g - seed.g), std::abs(p.b - seed.b), std::abs(p.a - seed.a)}) <= tol;
    };
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    const auto mark = [&](int px, int py) { inside[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)] = 1; };
    const auto marked = [&](int px, int py) {
        return inside[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)] != 0;
    };
    if (connected) {
        // Horizontal spans, four neighbours, bounds and visited checks (§10.1).
        std::vector<std::pair<int, int>> stack{{x, y}};
        std::vector<std::uint8_t> visited(inside.size(), 0);
        const auto visit = [&](int px, int py) {
            std::uint8_t& v = visited[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)];
            if (v) return false;
            v = 1;
            return true;
        };
        while (!stack.empty()) {
            auto [sx, sy] = stack.back();
            stack.pop_back();
            if (marked(sx, sy) || !matches(sx, sy)) continue;
            int left = sx;
            while (left > 0 && !marked(left - 1, sy) && matches(left - 1, sy)) --left;
            int right = sx;
            while (right + 1 < width && !marked(right + 1, sy) && matches(right + 1, sy)) ++right;
            for (int px = left; px <= right; ++px) mark(px, sy);
            for (const int ny : {sy - 1, sy + 1}) {
                if (ny < 0 || ny >= height) continue;
                for (int px = left; px <= right; ++px) {
                    if (!marked(px, ny) && visit(px, ny)) stack.emplace_back(px, ny);
                }
            }
        }
    } else {
        for (int py = 0; py < height; ++py) {
            for (int px = 0; px < width; ++px) {
                if (matches(px, py)) mark(px, py);
            }
        }
    }
    std::map<std::pair<int, int>, std::shared_ptr<MaskTile>> built;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            if (!marked(px, py)) continue;
            auto& t = built[{px / kTileSize, py / kTileSize}];
            if (!t) t = std::make_shared<MaskTile>();
            t->bytes[static_cast<std::size_t>(py % kTileSize) * kTileSize + static_cast<std::size_t>(px % kTileSize)] = 255;
        }
    }
    for (auto& [k, t] : built) mask.setTile(k.first, k.second, std::move(t));
    return mask;
}

std::vector<TileEdit> fillRegion(const TileSource& layer, const SelectionMask& region,
                                 const std::optional<SelectionMask>& selection, const Pixel& ink, bool alphaLock,
                                 int width, int height) {
    TileReader before(layer);
    std::set<std::pair<int, int>> keys;
    for (const auto& [k, t] : region.tiles()) keys.insert(k);
    const float inkAlpha = ink.a;
    const Pixel straightInk = inkAlpha > 0 ? Pixel{ink.r / inkAlpha, ink.g / inkAlpha, ink.b / inkAlpha, 1.0f} : Pixel{};
    return rebuild(before, keys, width, height, [&](int x, int y) {
        const Pixel p = before.at(x, y);
        const float w = region.at(x, y) / 255.0f * maskAt(selection, x, y);
        if (w <= 0.0f) return p;
        if (alphaLock) {
            // §7: (a0·mix(straight(old), ink, w·inkAlpha), a0); an empty pixel stays empty.
            if (p.a <= 0.0f) return Pixel{};
            const float k = w * inkAlpha;
            const auto mix = [&](float old, float c) { return p.a * (old / p.a + (c - old / p.a) * k); };
            return Pixel{mix(p.r, straightInk.r), mix(p.g, straightInk.g), mix(p.b, straightInk.b), p.a};
        }
        return over(scaled(ink, w), p);
    });
}

std::vector<TileEdit> moveLayer(const TileSource& layer, const std::vector<std::pair<int, int>>& present,
                                const std::optional<SelectionMask>& mask, int dx, int dy, int width, int height) {
    if (dx == 0 && dy == 0) return {};  // §10.1: a zero move returns the base verbatim
    TileReader before(layer);
    std::set<std::pair<int, int>> keys;
    const auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    for (const auto& [tx, ty] : present) {
        keys.insert({tx, ty});
        for (const int ox : {0, 1}) {
            for (const int oy : {0, 1}) {
                keys.insert({floorDiv(tx * kTileSize + dx, kTileSize) + ox, floorDiv(ty * kTileSize + dy, kTileSize) + oy});
            }
        }
    }
    return rebuild(before, keys, width, height, [&](int x, int y) {
        const Pixel stay = scaled(before.at(x, y), 1.0f - maskAt(mask, x, y));
        const int sx = x - dx;
        const int sy = y - dy;
        // Outside the source canvas the layer is transparent (canonical tiles are zero there).
        const Pixel moved = scaled(before.at(sx, sy), maskAt(mask, sx, sy));
        return mask ? over(moved, stay) : moved;  // without a mask the whole layer moves, exactly
    });
}

std::vector<TileEdit> clearMasked(const TileSource& layer, const SelectionMask& mask, int width, int height) {
    TileReader before(layer);
    std::set<std::pair<int, int>> keys;
    for (const auto& [k, t] : mask.tiles()) keys.insert(k);
    return rebuild(before, keys, width, height, [&](int x, int y) { return scaled(before.at(x, y), 1.0f - mask.at(x, y) / 255.0f); });
}

MaskedSplit splitMasked(const TileSource& layer, const std::vector<std::pair<int, int>>& present, const SelectionMask& mask,
                        int width, int height) {
    MaskedSplit split;
    for (const auto& [tx, ty] : present) {
        const TileContent source = layer(tx, ty);
        if (!source) continue;
        const MaskRef m = mask.tile(tx, ty);
        if (!m) {
            split.base[{tx, ty}] = source;
            continue;
        }
        auto base = std::make_shared<TileData>();
        auto floating = std::make_shared<TileData>();
        for (int y = 0; y < kTileSize; ++y) {
            for (int x = 0; x < kTileSize; ++x) {
                const Pixel p = readPixel(*source, x, y);
                const float k = m->bytes[static_cast<std::size_t>(y) * kTileSize + static_cast<std::size_t>(x)] / 255.0f;
                writePixel(*base, x, y, scaled(p, 1.0f - k));
                writePixel(*floating, x, y, scaled(p, k));
            }
        }
        if (TileContent b = finish(std::move(base), width, height, tx, ty)) split.base[{tx, ty}] = std::move(b);
        if (TileContent f = finish(std::move(floating), width, height, tx, ty)) split.floating[{tx, ty}] = std::move(f);
    }
    return split;
}

Image8 extract(const TileSource& layer, const std::vector<std::pair<int, int>>& present,
               const std::optional<SelectionMask>& mask, int width, int height) {
    TileReader reader(layer);
    PixelBox box;
    if (mask) {
        box = mask->bounds();
    } else {
        // The layer's content: the bounds of its nonzero alpha.
        bool any = false;
        for (const auto& [tx, ty] : present) {
            const TileContent& t = reader.tile(tx, ty);
            if (!t) continue;
            for (int y = 0; y < kTileSize; ++y) {
                for (int x = 0; x < kTileSize; ++x) {
                    if (t->halfs[pixelIndex(x, y) + 3] == 0) continue;
                    const int px = tx * kTileSize + x;
                    const int py = ty * kTileSize + y;
                    box = any ? PixelBox{std::min(box.x0, px), std::min(box.y0, py), std::max(box.x1, px + 1), std::max(box.y1, py + 1)}
                              : PixelBox{px, py, px + 1, py + 1};
                    any = true;
                }
            }
        }
    }
    box = {std::max(box.x0, 0), std::max(box.y0, 0), std::min(box.x1, width), std::min(box.y1, height)};
    Image8 image;
    if (box.empty()) return image;
    image.x = box.x0;
    image.y = box.y0;
    image.width = box.width();
    image.height = box.height();
    image.rgba.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4, 0);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const Pixel p = scaled(reader.at(box.x0 + x, box.y0 + y), maskAt(mask, box.x0 + x, box.y0 + y));
            std::uint8_t* out = &image.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4];
            const std::uint8_t a = toByte(p.a);
            if (a == 0) continue;  // rgb 0 at a = 0 (§10.3)
            out[0] = toByte(linearToSrgb(p.r / p.a));
            out[1] = toByte(linearToSrgb(p.g / p.a));
            out[2] = toByte(linearToSrgb(p.b / p.a));
            out[3] = a;
        }
    }
    return image;
}

std::vector<TileEdit> placeImage(const Image8& image, int width, int height) {
    std::map<std::pair<int, int>, std::shared_ptr<TileData>> built;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int px = image.x + x;
            const int py = image.y + y;
            if (px < 0 || py < 0 || px >= width || py >= height) continue;
            const std::uint8_t* in = &image.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4];
            if (in[3] == 0) continue;
            const auto pma = premultipliedLinear(in[0] / 255.0, in[1] / 255.0, in[2] / 255.0, in[3] / 255.0);
            auto& t = built[{px / kTileSize, py / kTileSize}];
            if (!t) t = std::make_shared<TileData>();
            writePixel(*t, px % kTileSize, py % kTileSize, {pma[0], pma[1], pma[2], pma[3]});
        }
    }
    std::vector<TileEdit> edits;
    for (auto& [k, t] : built) {
        if (TileContent after = finish(std::move(t), width, height, k.first, k.second)) edits.push_back({k.first, k.second, std::move(after)});
    }
    return edits;
}

}  // namespace chroma::core

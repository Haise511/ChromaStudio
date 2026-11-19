#include "core/Compositor.hpp"

#include "core/Half.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace chroma::core {
namespace {

struct V3 {
    float r, g, b;
};

float sat(float x) { return std::clamp(x, 0.0f, 1.0f); }
float lum(V3 v) { return v.r * 0.30f + v.g * 0.59f + v.b * 0.11f; }
float maxOf(V3 v) { return std::max({v.r, v.g, v.b}); }
float minOf(V3 v) { return std::min({v.r, v.g, v.b}); }
float span3(V3 v) { return maxOf(v) - minOf(v); }

V3 withSat(V3 c, float target) {
    const float d = span3(c);
    if (!(d > 0.0f)) return {0.0f, 0.0f, 0.0f};
    const float n = minOf(c);
    const float k = target / d;
    return {(c.r - n) * k, (c.g - n) * k, (c.b - n) * k};
}

V3 withLum(V3 c, float target) {
    const float shift = target - lum(c);
    c = {c.r + shift, c.g + shift, c.b + shift};
    const float n = minOf(c);
    const float x = maxOf(c);
    if (n < 0.0f) {
        const float k = target / (target - n);
        c = {target + (c.r - target) * k, target + (c.g - target) * k, target + (c.b - target) * k};
    }
    if (x > 1.0f) {
        const float k = (1.0f - target) / (x - target);
        c = {target + (c.r - target) * k, target + (c.g - target) * k, target + (c.b - target) * k};
    }
    return {sat(c.r), sat(c.g), sat(c.b)};
}

float dodge(float b, float s) {
    if (b == 0.0f) return 0.0f;
    return s >= 1.0f ? 1.0f : std::min(1.0f, b / (1.0f - s));
}

float burn(float b, float s) {
    if (b >= 1.0f) return 1.0f;
    return s <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - b) / s);
}

float overlay(float b, float s) { return b <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s); }

float vivid(float b, float s) { return s < 0.5f ? burn(b, 2.0f * s) : dodge(b, 2.0f * s - 1.0f); }

float channel(float b, float s, int mode) {
    switch (mode) {
    case 1: return b * s;
    case 2: return b + s - b * s;
    case 3: return overlay(b, s);
    case 4: return std::min(1.0f, b + s);
    case 5: return dodge(b, s);
    case 6: return std::max(0.0f, b + s - 1.0f);
    case 7: return std::min(b, s);
    case 8: return std::max(b, s);
    case 9: return burn(b, s);
    case 12: {
        const float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
        return s <= 0.5f ? b - (1.0f - 2.0f * s) * b * (1.0f - b) : b + (2.0f * s - 1.0f) * (d - b);
    }
    case 13: return overlay(s, b);
    case 14: return vivid(b, s);
    case 15: return sat(b + 2.0f * s - 1.0f);
    case 16: return s < 0.5f ? std::min(b, 2.0f * s) : std::max(b, 2.0f * s - 1.0f);
    case 17: return vivid(b, s) >= 0.5f ? 1.0f : 0.0f;  // step(.5, x)
    case 18: return std::abs(b - s);
    case 19: return b + s - 2.0f * b * s;
    case 20: return std::max(0.0f, b - s);
    case 21: return s == 0.0f ? (b == 0.0f ? 0.0f : 1.0f) : std::min(b / s, 1.0f);
    case 26: return 1.0f - b;
    default: return s;
    }
}

V3 blend(V3 b, V3 s, int mode) {
    switch (mode) {
    case 10: return lum(s) < lum(b) ? s : b;
    case 11: return lum(s) > lum(b) ? s : b;
    case 22: return withLum(withSat(s, span3(b)), lum(b));
    case 23: return withLum(withSat(b, span3(s)), lum(b));
    case 24: return withLum(s, lum(b));
    case 25: return withLum(b, lum(s));
    default: return {channel(b.r, s.r, mode), channel(b.g, s.g, mode), channel(b.b, s.b, mode)};
    }
}

using Buffer = std::vector<Pixel>;

class Composer {
public:
    Composer(const LayerTree& tree, std::size_t count, const LayerFetch& fetch)
        : tree_(tree), count_(count), fetch_(fetch) {}

    /// The composite of `parent`'s children (0: the root) into `out`.
    void children(std::uint64_t parent, std::span<Pixel> out) {
        std::fill(out.begin(), out.end(), Pixel{});
        const std::vector<std::uint64_t>& list = tree_.children(parent);
        Buffer src(count_), chain(count_), clipped(count_);
        for (std::size_t i = 0; i < list.size();) {
            const LayerNode& node = *tree_.find(list[i]);
            std::size_t j = i + 1;
            while (j < list.size() && tree_.find(list[j])->props.clipToBase) ++j;
            if (node.props.clipToBase) {  // orphan: nothing below to clip to
                ++i;
                continue;
            }
            if (!node.props.visible || !content(node, src)) {  // a hidden or empty base hides its chain
                i = j;
                continue;
            }
            const int mode = node.props.blendId;
            if (j == i + 1) {
                const auto opacity = static_cast<float>(node.props.opacity);
                for (std::size_t p = 0; p < count_; ++p) out[p] = quantize(blendPixel(out[p], src[p], opacity, mode));
                i = j;
                continue;
            }
            // Clip chain: the base colour at unit coverage, the clipped layers on top of it, then the
            // base silhouette aBase·opacityBase, then the chain over the scene in the base mode.
            for (std::size_t p = 0; p < count_; ++p) {
                const Pixel& s = src[p];
                chain[p] = s.a > 0.0f ? quantize({std::clamp(s.r / s.a, 0.0f, 1.0f), std::clamp(s.g / s.a, 0.0f, 1.0f),
                                                  std::clamp(s.b / s.a, 0.0f, 1.0f), 1.0f})
                                      : Pixel{0.0f, 0.0f, 0.0f, 1.0f};
            }
            for (std::size_t k = i + 1; k < j; ++k) {
                const LayerNode& top = *tree_.find(list[k]);
                if (!top.props.visible || !content(top, clipped)) continue;
                const auto opacity = static_cast<float>(top.props.opacity);
                for (std::size_t p = 0; p < count_; ++p) {
                    chain[p] = quantize(blendPixel(chain[p], clipped[p], opacity, top.props.blendId));
                }
            }
            const float baseOpacity = sat(static_cast<float>(node.props.opacity));
            for (std::size_t p = 0; p < count_; ++p) {
                const float k = src[p].a * baseOpacity;
                const Pixel silhouette = quantize({chain[p].r * k, chain[p].g * k, chain[p].b * k, k});
                out[p] = quantize(blendPixel(out[p], silhouette, 1.0f, mode));
            }
            i = j;
        }
    }

private:
    bool content(const LayerNode& node, Buffer& into) {
        if (node.kind == NodeKind::Group) {
            children(node.id, into);
            return true;
        }
        return fetch_(node.id, into);
    }

    const LayerTree& tree_;
    std::size_t count_;
    const LayerFetch& fetch_;
};

}  // namespace

Pixel blendPixel(const Pixel& dst, const Pixel& src, float opacity, int mode) {
    const float a = src.a * sat(opacity);
    const float h = dst.a;
    if (a <= 0.0f) return dst;
    const V3 s{std::clamp(src.r / src.a, 0.0f, 1.0f), std::clamp(src.g / src.a, 0.0f, 1.0f),
               std::clamp(src.b / src.a, 0.0f, 1.0f)};
    const V3 b = h > 0.0f ? V3{std::clamp(dst.r / h, 0.0f, 1.0f), std::clamp(dst.g / h, 0.0f, 1.0f),
                               std::clamp(dst.b / h, 0.0f, 1.0f)}
                          : V3{0.0f, 0.0f, 0.0f};
    const float outA = a + h * (1.0f - a);
    const V3 m = blend(b, s, mode);
    const auto mix = [&](float d, float sv, float bv) {
        return std::clamp((1.0f - a) * d + a * (1.0f - h) * sv + a * h * bv, 0.0f, outA);
    };
    return {mix(dst.r, s.r, m.r), mix(dst.g, s.g, m.g), mix(dst.b, s.b, m.b), outA};
}

Pixel quantize(const Pixel& p) {
    const auto q = [](float v) { return halfToFloat(floatToHalf(v)); };
    return {q(p.r), q(p.g), q(p.b), q(p.a)};
}

void compose(const LayerTree& tree, std::span<Pixel> out, const LayerFetch& fetch) {
    Composer(tree, out.size(), fetch).children(0, out);
}

}  // namespace chroma::core

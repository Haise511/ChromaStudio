#pragma once
// ChromaStudio Engine · core · colour (spec §2, §11.1).
//
// The document stores linear sRGB with premultiplied alpha; encoded sRGB exists only at the edges
// (colour pickers, display, files). HSV is defined on encoded sRGB and converted to RGB through six
// sectors (spec §11.1 "Круг HSV").

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>

namespace chroma::core {

inline double srgbToLinear(double e) { return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4); }

inline double linearToSrgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

/// Encoded sRGB colour with straight alpha → canonical linear premultiplied pixel P = (a·r, a·g, a·b, a).
inline std::array<float, 4> premultipliedLinear(double r, double g, double b, double a) {
    const auto channel = [a](double e) { return static_cast<float>(a * srgbToLinear(e)); };
    return {channel(r), channel(g), channel(b), static_cast<float>(a)};
}

/// Encoded sRGB, components in [0, 1].
using Rgb = std::array<double, 3>;

struct Hsv {
    double h = 0.0;  // [0, 1): red at 0, counter-clockwise
    double s = 0.0;
    double v = 0.0;
};

/// §11.1: k = floor(6H), f = 6H − k, c = VS, x = c(1 − |mod(6H, 2) − 1|), m = V − c; sectors
/// (c,x,0), (x,c,0), (0,c,x), (0,x,c), (x,0,c), (c,0,x), plus m per channel.
inline Rgb hsvToRgb(const Hsv& hsv) {
    const double h = hsv.h - std::floor(hsv.h);
    const double s = std::clamp(hsv.s, 0.0, 1.0);
    const double v = std::clamp(hsv.v, 0.0, 1.0);
    const double h6 = 6.0 * h;
    const int k = static_cast<int>(std::floor(h6)) % 6;
    const double c = v * s;
    const double x = c * (1.0 - std::abs(std::fmod(h6, 2.0) - 1.0));
    const double m = v - c;
    Rgb rgb{};
    switch (k) {
    case 0: rgb = {c, x, 0}; break;
    case 1: rgb = {x, c, 0}; break;
    case 2: rgb = {0, c, x}; break;
    case 3: rgb = {0, x, c}; break;
    case 4: rgb = {x, 0, c}; break;
    default: rgb = {c, 0, x}; break;
    }
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

/// The inverse; a grey keeps `hueWhenGrey` (§11.1: at S = 0 remember the last meaningful H).
inline Hsv rgbToHsv(const Rgb& rgb, double hueWhenGrey = 0.0) {
    const double r = std::clamp(rgb[0], 0.0, 1.0);
    const double g = std::clamp(rgb[1], 0.0, 1.0);
    const double b = std::clamp(rgb[2], 0.0, 1.0);
    const double hi = std::max({r, g, b});
    const double lo = std::min({r, g, b});
    const double delta = hi - lo;
    Hsv hsv;
    hsv.v = hi;
    hsv.s = hi > 0.0 ? delta / hi : 0.0;
    if (delta <= 0.0) {
        hsv.h = hueWhenGrey;
        return hsv;
    }
    double h = 0.0;
    if (hi == r) {
        h = (g - b) / delta;
    } else if (hi == g) {
        h = 2.0 + (b - r) / delta;
    } else {
        h = 4.0 + (r - g) / delta;
    }
    h /= 6.0;
    hsv.h = h - std::floor(h);
    return hsv;
}

/// §11.1 ring: H = mod(atan2(−dy, dx) / 2π, 1); dy grows downwards on screen.
inline double hueFromOffset(double dx, double dy) {
    const double h = std::atan2(-dy, dx) / (2.0 * std::numbers::pi);
    return h - std::floor(h);
}

inline std::uint8_t toByte(double unit) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
}

/// "#RRGGBB" of an encoded sRGB colour.
inline std::string toHex(const Rgb& rgb) {
    return std::format("#{:02X}{:02X}{:02X}", toByte(rgb[0]), toByte(rgb[1]), toByte(rgb[2]));
}

/// Accepts "#RRGGBB", "RRGGBB", "#RGB" or "RGB" (hex digits in either case).
inline std::optional<Rgb> parseHex(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 3) return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Rgb rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        int value = 0;
        if (text.size() == 6) {
            const int hi = digit(text[2 * i]);
            const int lo = digit(text[2 * i + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            value = hi * 16 + lo;
        } else {
            const int d = digit(text[i]);
            if (d < 0) return std::nullopt;
            value = d * 17;
        }
        rgb[i] = value / 255.0;
    }
    return rgb;
}

}  // namespace chroma::core

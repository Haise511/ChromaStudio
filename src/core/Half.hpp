#pragma once
// ChromaStudio Engine · core · IEEE 754 binary16 (spec §2: results are rounded to binary16,
// to nearest with ties to even; tiles are stored as little-endian binary16 RGBA).

#include <bit>
#include <cstdint>

namespace chroma::core {

/// float → binary16, round to nearest even; overflow → ±inf, NaN stays NaN.
inline std::uint16_t floatToHalf(float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xFFu;
    std::uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent == 0xFFu) {  // inf / NaN
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u | (mantissa >> 13) : 0u));
    }
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow
    if (e <= 0) {                                                    // subnormal or zero
        if (e < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        const std::uint32_t half = mantissa >> shift;
        const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const std::uint32_t midpoint = 1u << (shift - 1);
        const std::uint32_t rounded = half + ((remainder > midpoint || (remainder == midpoint && (half & 1u))) ? 1u : 0u);
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
    const std::uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++half;  // may carry into the exponent
    return static_cast<std::uint16_t>(sign | half);
}

inline float halfToFloat(std::uint16_t value) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(value) & 0x8000u) << 16;
    const std::uint32_t exponent = (value >> 10) & 0x1Fu;
    std::uint32_t mantissa = value & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // subnormal: normalise
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

}  // namespace chroma::core

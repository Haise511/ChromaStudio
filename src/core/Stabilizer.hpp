#pragma once
// ChromaStudio Engine · core · real-time position filters (spec §3 "Фильтры в реальном времени").
//
//   off     positions pass through.
//   ema     per axis a = −expm1(−Δt/τ), yₙ = yₙ₋₁ + a(xₙ − yₙ₋₁); the first packet or τ = 0 passes.
//   window  time-weighted mean over [t − W, t] with linear history between samples (trapezoids),
//           keeping one sample left of the window.
// A pause of more than 250 ms re-initialises the filter at the current position. Samples with the
// same time replace each other before filtering (the last position wins); older samples are dropped.

#include "core/Geometry.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace chroma::core {

enum class StabilizerMethod : std::uint8_t { Off, Ema, Window };

struct StabilizerSettings {
    StabilizerMethod method = StabilizerMethod::Off;
    double timeMs = 0.0;  // τ for ema, W for window; [0, 200]
    friend bool operator==(const StabilizerSettings&, const StabilizerSettings&) = default;
};

struct TimedXY {
    double ms, x, y;
};

/// §3 listing: time-weighted mean of a chronological contact range that includes one sample before
/// the window. Time in ms.
std::array<double, 2> windowMean(std::span<const TimedXY> v, double widthMs);

class Stabilizer {
public:
    explicit Stabilizer(StabilizerSettings settings);

    /// Filters one raw position. Equal time replaces the previous sample; earlier time is ignored.
    V push(double timeMs, V raw);

private:
    StabilizerSettings settings_;
    bool started_ = false;
    double lastMs_ = 0.0;
    double lastDtMs_ = 0.0;  // Δt that produced output_
    V output_{};
    V beforeLast_{};  // ema state before the last sample, for same-time replacement
    std::vector<TimedXY> history_;
};

}  // namespace chroma::core

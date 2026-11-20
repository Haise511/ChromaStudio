#pragma once
// ChromaStudio Engine · project_io · PNG export through WIC (spec §10.3 "Обмен файлами").
// Straight-alpha sRGB, 8 bits per channel, RGB exactly zero where alpha is zero; the paper,
// checkerboard and guides are not part of the image. Written to a temporary file in the destination
// directory and renamed over the target, so a failed export never leaves a half-written file.
// The calling thread must have initialised COM.

#include "core/TileData.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace chroma::project {

/// Tile (tx, ty) of the flattened image, or nullptr for a transparent tile.
using TileSource = std::function<std::shared_ptr<const core::TileData>(int tx, int ty)>;

/// Returns an empty string on success, otherwise a message for the user.
std::string exportPng(const std::filesystem::path& path, int width, int height, const TileSource& tiles);

/// §10.3: the flattened image composed over an opaque background (encoded sRGB, like the canvas),
/// baseline JPEG at quality 0.92, 8 bits.
std::string exportJpeg(const std::filesystem::path& path, int width, int height, const TileSource& tiles,
                       const std::array<float, 3>& background);

/// Straight sRGB RGBA8 rows, top to bottom → PNG bytes in memory (thumbnails); empty on failure.
std::vector<std::byte> encodePng(int width, int height, std::span<const std::uint8_t> rgba);

/// One canvas pixel over an opaque background → RGB8 (exposed for tests).
void pmaOverToSrgb8(const std::uint16_t* halfs, const std::array<float, 3>& background, std::uint8_t* rgb);

/// One canvas pixel → straight sRGB RGBA8, RGB exactly zero where alpha is zero (exposed for tests).
void pmaToStraightSrgb8(const std::uint16_t* halfs, std::uint8_t* rgba);

}  // namespace chroma::project

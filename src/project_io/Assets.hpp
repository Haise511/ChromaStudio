#pragma once
// ChromaStudio Engine · project_io · local resources by content hash (spec §13, §14).
//
// Masks are referenced by the SHA-256 of their file bytes. The store indexes directories on first
// use (assets/textures, assets/materials, the user's library) and keeps blobs added at run time (masks
// of an opened project). Images decode through WIC as straight RGBA8 with numeric channel values (no
// colour management); a mask is the R8 of one §14.2 channel: a, r·a or (1 − 0.2126r − 0.7152g − 0.0722b)·a.

#include "core/BrushProfile.hpp"
#include "core/MaskImage.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chroma::project {

/// Lowercase hex SHA-256 (BCrypt).
std::string sha256Hex(std::span<const std::byte> bytes);

struct ImageRgba {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // straight, rows top to bottom
};

/// PNG or JPEG; nullopt with a reason. COM must be initialised on the calling thread.
std::optional<ImageRgba> decodeImage(std::span<const std::byte> bytes, std::string& error);

core::MaskImage maskFrom(const ImageRgba& image, core::MaskChannel channel);

/// §10.3 "Обмен файлами": a PNG (8/16 bits, straight alpha) or JPEG for a new layer. The EXIF
/// orientation is applied, an embedded ICC profile is converted to sRGB (WIC colour transform), no
/// profile means sRGB. At most 32768 px per axis.
struct PictureInfo {
    int orientation = 1;        // EXIF 1–8
    bool profileConverted = false;
    bool profileIgnored = false;  // a profile WIC could not convert; the numbers are taken as sRGB
};
std::optional<ImageRgba> decodePicture(std::span<const std::byte> bytes, std::string& error, PictureInfo& info);

/// EXIF orientation 1–8 → the upright image (exposed for tests).
ImageRgba orient(const ImageRgba& image, int orientation);

std::optional<std::vector<std::byte>> readFileBytes(const std::filesystem::path& path);

class AssetStore {
public:
    /// Files of the directory (not recursive) become available by hash and by file stem.
    void addDirectory(const std::filesystem::path& directory);
    /// Bytes known by their hash; returns the hash.
    std::string addBlob(std::vector<std::byte> bytes);

    [[nodiscard]] bool contains(const std::string& sha);
    [[nodiscard]] std::optional<std::vector<std::byte>> bytes(const std::string& sha);
    /// The hash of a file by its stem, e.g. "texture_7" (local aliases, §13).
    [[nodiscard]] std::optional<std::string> byName(const std::string& stem);
    /// The decoded mask, cached; nullptr when the hash is unknown or the image does not decode.
    std::shared_ptr<const core::MaskImage> mask(const std::string& sha, core::MaskChannel channel);

private:
    void index();

    std::vector<std::filesystem::path> directories_;
    bool indexed_ = false;
    std::map<std::string, std::filesystem::path> files_;  // sha → path
    std::map<std::string, std::string> names_;            // stem → sha
    std::map<std::string, std::vector<std::byte>> blobs_;
    std::map<std::pair<std::string, int>, std::shared_ptr<const core::MaskImage>> masks_;
};

}  // namespace chroma::project

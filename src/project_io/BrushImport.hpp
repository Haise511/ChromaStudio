#pragma once
// ChromaStudio Engine · project_io · BrushImportEngine and the artist's library (spec §14).
//
// One entry for files and pasted text. The extension only filters the dialog; the signature and the
// structure choose the decoder:
//   §6 JSON (a profile, or a catalogue of one)  → "mapped", every supported property kept
//   PresetFormat v1 (§14.2), as JSON, Base64/Base64url, a data: URI or one preset=/data= parameter
//                                                 → "mapped", units normalised
//   IPBZ binary (versions 80100, 120200)          → "basic": name and diameter, the payload kept
//   PNG cards with a QR code                      → refused: the local QR decoder is not in this build
// Nothing on disk changes until saveUserBrush, which writes user_brushes.json atomically and keeps
// the source payload as sources/<sha256>.bin.

#include "core/BrushProfile.hpp"
#include "project_io/Assets.hpp"
#include "project_io/Json.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chroma::project {

struct ImportResult {
    std::optional<core::BrushProfile> profile;
    std::string fidelity;           // "mapped" or "basic"
    std::string error;              // the reason, in Russian
    std::vector<std::byte> source;  // the payload as read
};

ImportResult importFromBytes(std::span<const std::byte> bytes, AssetStore& assets);
ImportResult importFromFile(const std::filesystem::path& path, AssetStore& assets);
/// A link or text: data: URIs, base64:/base64url:, one preset= or data= after ? or #, plain or
/// percent-encoded JSON, plain Base64. A link to a page only is refused (no local data).
ImportResult importFromPayload(std::string_view text, AssetStore& assets);

/// §14.2 PresetFormat v1 → a profile based on core.<family>; throws std::runtime_error.
core::BrushProfile mapPresetFormat(const json::Value& value, AssetStore& assets);

/// §14.2: τ = −sampleMs / log1p(−α), 0 for α = 1.
double emaTau(double alpha, double sampleMs);

/// §14.2: the user.* ID from the SHA-256 of the normalised profile.
std::string userBrushId(const core::BrushProfile& profile);

struct LibraryEntry {
    core::BrushProfile profile;
    std::string sourceSha256;
    std::string fidelity;
};

/// user_brushes.json of a library directory; a missing file is an empty library.
std::vector<LibraryEntry> loadLibrary(const std::filesystem::path& directory, std::string& error);

/// Adds the import to the library (replacing an entry with the same ID); empty on success. On any
/// failure the previous user_brushes.json stays as it was.
std::string saveUserBrush(const std::filesystem::path& directory, const ImportResult& result);

}  // namespace chroma::project

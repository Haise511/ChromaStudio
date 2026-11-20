#pragma once
// ChromaStudio Engine · project_io · native .rawproj documents (spec §10.3).
//
//   manifest.json              format, version, size, colour space, layer tree, tile list
//   tiles/<layer>/<tx>_<ty>.bin  128 × 128 little-endian binary16 RGBA, exactly 131 072 bytes
//
// Colour: "srgb-pma-f16le" — encoded sRGB, premultiplied, the space the canvas blends in. Files of
// version 0.1.0 say "linear-srgb-pma-f16le" (linear light); they open converted, every tile and
// vector colour, and are flagged as migrated so the shell saves them as a new file (§10.3).
//
// Opening validates the whole archive into a temporary snapshot before the caller replaces its
// document: limits and paths first, then the manifest, then every tile. Saving writes a temporary
// file in the destination directory, re-reads it to check the central directory and every CRC,
// flushes and closes it, then publishes it with ReplaceFileW (existing file, with a backup) or
// MoveFileExW (new file). On any failure the previous version stays where it was.
//
//   selection/<tx>_<ty>.bin     the active selection's R8 tiles, 16 384 bytes each (§10.3)
//   vectors/<layer>.json        the objects of a vector layer in z order (§8, §10.3)
//
// Vector layers keep only their objects in the file; their pixels are replayed from the geometry.
// This version reads and writes documents without assets; files that contain them are rejected with
// a message before anything is replaced.

#include "core/LayerTree.hpp"
#include "core/Selection.hpp"
#include "core/Vector.hpp"
#include "core/TileData.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chroma::project {

enum class LayerType : std::uint8_t { Raster, Vector, Group };

struct LayerInfo {
    std::uint64_t id = 1;
    std::optional<std::uint64_t> parent;  // a group, or the root
    LayerType type = LayerType::Raster;
    std::string name = "Слой 1";
    bool visible = true;
    double opacity = 1.0;
    int blendId = 0;
    bool clipToBase = false;
    bool alphaLock = false;
    bool locked = false;
};

struct TileEntry {
    std::uint64_t layer = 1;
    int tx = 0;
    int ty = 0;
    std::shared_ptr<const core::TileData> data;
};

/// An immutable revision of a document: everything a .rawproj holds.
struct ProjectSnapshot {
    int width = 1;
    int height = 1;
    int dpi = 96;
    std::uint64_t nextId = 2;
    std::uint64_t active = 1;
    std::vector<LayerInfo> layers;  // bottom to top among siblings
    std::vector<TileEntry> tiles;   // transparent tiles are simply absent
    std::optional<core::SelectionMask> selection;  // nullopt: no active selection (§10.3)
    std::map<std::uint64_t, core::VectorLayer> vectors;  // objects of every vector layer, bottom to top
    std::shared_ptr<const std::vector<std::byte>> thumbnailPng;  // optional thumbnail.png, written as is
    bool migratedFromLinear = false;  // read from a 0.1.0 linear-light file and converted (not written)
};

/// Identity of a file on disk, to notice changes made by another process (spec §10.3).
struct FileIdentity {
    std::uint32_t volume = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t size = 0;
    std::uint64_t lastWrite = 0;
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

std::optional<FileIdentity> identifyFile(const std::filesystem::path& path);

struct SaveResult {
    bool ok = false;
    std::string error;  // for the user, Russian
    std::optional<FileIdentity> identity;
};

/// Test hook: return true to make saving fail at that stage (spec §12.1 "прерывание до replace").
enum class SaveStage : std::uint8_t { Written, Verified, BeforeReplace };
using SaveFault = std::function<bool(SaveStage)>;

SaveResult saveProject(const std::filesystem::path& path, const ProjectSnapshot& snapshot, const SaveFault& fault = {});

struct LoadResult {
    std::optional<ProjectSnapshot> snapshot;
    std::string error;  // for the user, Russian
    std::optional<FileIdentity> identity;
};

LoadResult loadProject(const std::filesystem::path& path);

/// Records by logical name (§10.3), e.g. from a recovery journal.
using Entries = std::map<std::string, std::vector<std::byte>>;
using EntryVisitor = std::function<void(const std::string& name, std::span<const std::byte> data)>;

/// Every record of a snapshot in file order, manifest first; the spans live during the call.
void forEachEntry(const ProjectSnapshot& snapshot, const EntryVisitor& visit);

/// Validates records exactly like loadProject validates a file (no identity).
LoadResult loadFromEntries(const Entries& entries);

/// manifest.json for a snapshot (exposed for tests).
std::string manifestJson(const ProjectSnapshot& snapshot);

/// A new document: one transparent raster layer.
ProjectSnapshot emptyProject(int width, int height);

/// The layer tree of a snapshot that loadProject accepted (or that setLayerTree wrote).
std::optional<core::LayerTree> layerTree(const ProjectSnapshot& snapshot, std::string& error);

/// Writes a tree into a snapshot: layers in preorder with siblings bottom to top (§10.3), the active
/// layer and nextId. Tiles are the caller's.
void setLayerTree(ProjectSnapshot& snapshot, const core::LayerTree& tree);

}  // namespace chroma::project

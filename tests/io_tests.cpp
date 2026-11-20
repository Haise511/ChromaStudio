// ChromaStudio Engine · engine_tests · project_io: CRC-32, DEFLATE, ZIP64, JSON, .rawproj, PNG export.
// Spec §10.3 and the "Файлы и отказ" row of §12.1: round-trip byte equality of canonical tiles,
// empty / filled / partial edge tiles, a corrupted CRC, interruption before replace.

#include "TestHarness.hpp"

#include "core/Half.hpp"
#include "core/TileData.hpp"
#include "project_io/Crc32.hpp"
#include "project_io/Inflate.hpp"
#include "project_io/Assets.hpp"
#include "project_io/BrushImport.hpp"
#include "project_io/BrushJson.hpp"
#include "project_io/Journal.hpp"
#include "project_io/Json.hpp"
#include "project_io/PngExport.hpp"
#include "project_io/Project.hpp"
#include "project_io/Zip.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

#include "DeflateVectors.inc"

namespace fs = std::filesystem;
using namespace chroma;

fs::path testDirectory() {
    static const fs::path dir = [] {
        fs::path d = fs::temp_directory_path() / std::format("chroma-io-tests-{}", GetCurrentProcessId());
        fs::remove_all(d);
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

std::span<const std::byte> bytes(const unsigned char* data, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(data), size};
}

std::vector<std::byte> fileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

void writeFile(const fs::path& path, std::span<const std::byte> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::string text(const std::vector<std::byte>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

// Writes a ZIP with the given entries through ZipWriter.
void writeZip(const fs::path& path, const std::vector<std::pair<std::string, std::vector<std::byte>>>& entries) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create test zip");
    project::ZipWriter writer(file);
    for (const auto& [name, data] : entries) writer.add(name, data);
    writer.finish();
    CloseHandle(file);
}

std::vector<std::byte> asBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

void setPixel(core::TileData& tile, int x, int y, float r, float g, float b, float a) {
    std::uint16_t* p = &tile.halfs[(static_cast<std::size_t>(y) * core::kTileSize + static_cast<std::size_t>(x)) * 4];
    p[0] = core::floatToHalf(r);
    p[1] = core::floatToHalf(g);
    p[2] = core::floatToHalf(b);
    p[3] = core::floatToHalf(a);
}

// 300 × 200: a full tile, an empty tile (absent) and a partial corner tile (44 × 72 inside).
project::ProjectSnapshot sampleProject() {
    project::ProjectSnapshot s = project::emptyProject(300, 200);
    auto full = std::make_shared<core::TileData>();
    for (int y = 0; y < core::kTileSize; ++y) {
        for (int x = 0; x < core::kTileSize; ++x) setPixel(*full, x, y, 0.25f * (x % 4) / 3.0f, 0.0f, 0.1f, 0.25f + 0.75f * (y % 2));
    }
    auto corner = std::make_shared<core::TileData>();
    for (int y = 0; y < 72; ++y) {
        for (int x = 0; x < 44; ++x) setPixel(*corner, x, y, 0.5f, 0.25f, 0.0f, 0.5f);
    }
    s.tiles.push_back({1, 0, 0, full});
    s.tiles.push_back({1, 2, 1, corner});
    s.layers.front().name = "Слой «тест»";
    s.layers.front().opacity = 0.75;
    return s;
}

bool sameSnapshot(const project::ProjectSnapshot& a, const project::ProjectSnapshot& b) {
    if (a.width != b.width || a.height != b.height || a.dpi != b.dpi || a.nextId != b.nextId || a.active != b.active ||
        a.layers.size() != b.layers.size() || a.tiles.size() != b.tiles.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        if (x.id != y.id || x.parent != y.parent || x.type != y.type || x.name != y.name || x.visible != y.visible ||
            x.opacity != y.opacity || x.blendId != y.blendId || x.clipToBase != y.clipToBase ||
            x.alphaLock != y.alphaLock || x.locked != y.locked) {
            return false;
        }
    }
    for (const auto& t : a.tiles) {
        const auto match = std::find_if(b.tiles.begin(), b.tiles.end(), [&](const project::TileEntry& u) {
            return u.layer == t.layer && u.tx == t.tx && u.ty == t.ty;
        });
        if (match == b.tiles.end() || !(*match->data == *t.data)) return false;
    }
    return true;
}

std::size_t leftoverFiles(const fs::path& dir, const std::wstring& needle) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().wstring().find(needle) != std::wstring::npos) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE(crc32KnownValues) {
    const std::string check = "123456789";
    CHECK(project::crc32(std::as_bytes(std::span(check))) == 0xCBF43926u);
    CHECK(project::crc32({}) == 0u);
    std::vector<std::byte> data(10000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    project::Crc32 split;
    split.update(std::span(data).first(3333));
    split.update(std::span(data).subspan(3333));
    CHECK(split.value() == project::crc32(data));
}

// Streams produced by .NET's DeflateStream.
TEST_CASE(inflateForeignStreams) {
    std::string expected;
    for (int i = 0; i < 30; ++i) expected += "ChromaStudio tiles are 128 by 128 pixels. ";
    CHECK(expected.size() == kTextSize);
    CHECK(text(project::inflateRaw(bytes(kTextDeflate, sizeof(kTextDeflate)), kTextSize)) == expected);

    std::vector<std::byte> generated(kGeneratedSize);
    for (std::size_t i = 0; i < kGeneratedSize; ++i) generated[i] = static_cast<std::byte>((i * i + i / 7) & 0xFF);
    CHECK(project::inflateRaw(bytes(kGeneratedDeflate, sizeof(kGeneratedDeflate)), kGeneratedSize) == generated);

    CHECK_THROWS(project::inflateRaw(bytes(kTextDeflate, sizeof(kTextDeflate)), kTextSize - 1));  // too much output
    CHECK_THROWS(project::inflateRaw(bytes(kTextDeflate, sizeof(kTextDeflate)), kTextSize + 1));  // too little
    CHECK_THROWS(project::inflateRaw(bytes(kGeneratedDeflate, sizeof(kGeneratedDeflate) / 2), kGeneratedSize));
}

TEST_CASE(inflateStoredBlock) {
    const unsigned char stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    CHECK(text(project::inflateRaw(bytes(stored, sizeof(stored)), 3)) == "abc");
    const unsigned char badLength[] = {0x01, 0x03, 0x00, 0xFC, 0xFE, 'a', 'b', 'c'};
    CHECK_THROWS(project::inflateRaw(bytes(badLength, sizeof(badLength)), 3));
}

// An archive written by .NET's ZipArchive: one deflated, one stored entry.
TEST_CASE(zipReadsForeignArchive) {
    const fs::path path = testDirectory() / "dotnet.zip";
    writeFile(path, bytes(kDotNetZip, sizeof(kDotNetZip)));
    const project::ZipReader zip(path);
    CHECK(zip.entries().size() == 2);
    const project::ZipEntry* hello = zip.find("hello.txt");
    const project::ZipEntry* raw = zip.find("folder/raw.bin");
    CHECK(hello && raw);
    if (!hello || !raw) return;
    CHECK(hello->method == 8);
    std::string expected;
    for (int i = 0; i < 40; ++i) expected += "hello zip ";
    CHECK(text(zip.read(*hello)) == expected);
    const auto rawBytes = zip.read(*raw);
    CHECK(rawBytes.size() == 100);
    bool sequence = rawBytes.size() == 100;
    for (std::size_t i = 0; i < rawBytes.size(); ++i) sequence = sequence && rawBytes[i] == static_cast<std::byte>(i);
    CHECK(sequence);
}

TEST_CASE(zipRoundTripAndDamage) {
    const fs::path path = testDirectory() / "roundtrip.zip";
    std::vector<std::byte> big(200000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i % 251);
    writeZip(path, {{"manifest.json", asBytes("{}")}, {"слой/данные.bin", big}, {"empty", {}}});
    {
        const project::ZipReader zip(path);
        CHECK(zip.entries().size() == 3);
        CHECK(zip.find("слой/данные.bin") && zip.read(*zip.find("слой/данные.bin")) == big);
        CHECK(zip.find("empty") && zip.read(*zip.find("empty")).empty());
    }
    // One flipped data byte is caught by the CRC; a truncated archive is refused.
    std::vector<std::byte> damaged = fileBytes(path);
    damaged[200] ^= std::byte{0x40};
    writeFile(path, damaged);
    {
        const project::ZipReader zip(path);
        CHECK_THROWS(zip.read(*zip.find("слой/данные.bin")));
    }
    damaged.resize(damaged.size() - 30);
    writeFile(path, damaged);
    CHECK_THROWS(project::ZipReader{path});

    HANDLE file = CreateFileW((testDirectory() / "names.zip").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    project::ZipWriter writer(file);
    CHECK_THROWS(writer.add("../evil", {}));
    CHECK_THROWS(writer.add("C:/evil", {}));
    CHECK_THROWS(writer.add("a\\b", {}));
    CHECK_THROWS(writer.add("a//b", {}));
    writer.finish();
    CloseHandle(file);
}

TEST_CASE(jsonStrict) {
    const json::Value v = json::parse(R"({"a": [1, 2.5, "x\u00e9\ud83d\ude00"], "b": null, "c": true, "d": -0.125e1})");
    CHECK(v.isObject());
    CHECK(v.find("a") && v.find("a")->items().size() == 3);
    CHECK(v.find("a")->items()[2].asString() == "x\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(v.find("b")->isNull());
    CHECK(v.find("c")->asBool());
    CHECK_NEAR(v.find("d")->asNumber(), -1.25, 0.0);
    CHECK(v.find("a")->items()[0].asInteger(0, 10) == 1);
    CHECK(!v.find("a")->items()[1].asInteger(0, 10));

    const json::Value again = json::parse(json::serialize(v));
    CHECK(json::serialize(again) == json::serialize(v));

    for (const char* bad : {R"({"a":1,"a":2})", "[01]", "[1,]", "{\"a\":\"\x01\"}", "\"\xFF\"", R"("\ud800")", "tru",
                            "[1] 2", "{\"a\" 1}"}) {
        CHECK_THROWS(json::parse(bad));
    }
    CHECK_THROWS(json::parse(std::string(200, '[') + std::string(200, ']')));
}

// §12.1: canonical tile bytes are equal after a round trip; empty tiles stay absent.
TEST_CASE(projectRoundTrip) {
    const fs::path path = testDirectory() / "Документ.rawproj";
    const project::ProjectSnapshot original = sampleProject();
    const project::SaveResult saved = project::saveProject(path, original);
    CHECK(saved.ok);
    if (!saved.ok) std::printf("    save error: %s\n", saved.error.c_str());
    CHECK(saved.identity.has_value());
    const project::LoadResult loaded = project::loadProject(path);
    CHECK(loaded.snapshot.has_value());
    if (!loaded.snapshot) {
        std::printf("    load error: %s\n", loaded.error.c_str());
        return;
    }
    CHECK(sameSnapshot(original, *loaded.snapshot));
    CHECK(loaded.snapshot->tiles.size() == 2);
    CHECK(leftoverFiles(testDirectory(), L".tmp-") == 0);
    CHECK(leftoverFiles(testDirectory(), L".bak") == 0);

    // The archive holds exactly the §10.3 records.
    const project::ZipReader zip(path);
    CHECK(zip.entries().size() == 3);
    CHECK(zip.find("manifest.json") && zip.find("tiles/1/0_0.bin") && zip.find("tiles/1/2_1.bin"));
}

// The minimal manifest printed in §10.3 opens as a 256 × 256 empty document.
TEST_CASE(specManifestOpens) {
    const std::string manifest = R"({
  "format": "chroma.rawproj",
  "major": 1, "minor": 0, "width": 256, "height": 256, "dpi": 96,
  "color": "linear-srgb-pma-f16le", "tileSize": 128, "nextId": "2",
  "active": "1", "layers": [
    {"id":"1","parent":null,"type":"raster","name":"Слой 1","visible":true,
     "opacity":1,"blendId":0,"clipToBase":false,"alphaLock":false,"locked":false}
  ],
  "tiles": [], "vectors": [], "assets": [], "selection": null
})";
    const fs::path path = testDirectory() / "minimal.rawproj";
    writeZip(path, {{"manifest.json", asBytes(manifest)}});
    const project::LoadResult loaded = project::loadProject(path);
    CHECK(loaded.snapshot.has_value());
    if (!loaded.snapshot) {
        std::printf("    load error: %s\n", loaded.error.c_str());
        return;
    }
    CHECK(loaded.snapshot->width == 256 && loaded.snapshot->height == 256);
    CHECK(loaded.snapshot->layers.size() == 1 && loaded.snapshot->layers[0].name == "Слой 1");
    CHECK(loaded.snapshot->tiles.empty());
}

TEST_CASE(projectRejectsInvalidFiles) {
    const project::ProjectSnapshot base = sampleProject();
    const std::string goodManifest = project::manifestJson(base);
    std::vector<std::pair<std::string, std::vector<std::byte>>> goodEntries = {{"manifest.json", asBytes(goodManifest)}};
    for (const auto& t : base.tiles) {
        goodEntries.push_back({std::format("tiles/1/{}_{}.bin", t.tx, t.ty), std::vector<std::byte>(core::kTileBytes)});
        std::memcpy(goodEntries.back().second.data(), t.data->halfs.data(), core::kTileBytes);
    }
    const fs::path path = testDirectory() / "invalid.rawproj";
    const auto expectError = [&](const std::vector<std::pair<std::string, std::vector<std::byte>>>& entries,
                                 const char* label) {
        writeZip(path, entries);
        const project::LoadResult r = project::loadProject(path);
        if (r.snapshot) ::test::fail(__FILE__, __LINE__, std::string("accepted: ") + label);
        CHECK(!r.error.empty());
    };
    const auto withManifest = [&](std::string manifest) {
        auto entries = goodEntries;
        entries[0].second = asBytes(manifest);
        return entries;
    };
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string m = goodManifest;
        const std::size_t at = m.find(from);
        if (at != std::string::npos) m.replace(at, from.size(), to);
        return withManifest(m);
    };

    writeZip(path, goodEntries);
    CHECK(project::loadProject(path).snapshot.has_value());  // the control case opens

    expectError(replaced("\"major\": 1", "\"major\": 2"), "foreign major version");
    expectError(replaced("\"minor\": 0", "\"minor\": 1, \"requiredFeatures\": [\"timeTravel\"]"), "unknown feature");
    expectError(replaced("\"tileSize\": 128", "\"tileSize\": 256"), "tile size");
    expectError(replaced("\"width\": 300", "\"width\": 40000"), "width limit");
    expectError(replaced("\"nextId\": \"2\"", "\"nextId\": \"1\""), "nextId not above the ids");
    expectError(replaced("\"active\": \"1\"", "\"active\": \"9\""), "missing active layer");
    expectError(replaced("\"parent\": null", "\"parent\": \"1\""), "parent is not a group");
    expectError(replaced("\"selection\": null", "\"selection\": {\"tiles\": [[0]]}"), "selection pair");
    expectError(withManifest("{ not json"), "broken manifest");

    auto unlisted = goodEntries;
    unlisted.push_back({"tiles/1/1_0.bin", std::vector<std::byte>(core::kTileBytes)});
    expectError(unlisted, "tile entry not in the manifest");
    auto stray = goodEntries;
    stray.push_back({"readme.txt", asBytes("hi")});
    expectError(stray, "entry outside the §10.3 table");
    auto shortTile = goodEntries;
    shortTile[1].second.resize(1000);
    expectError(shortTile, "short tile");

    auto nanTile = goodEntries;
    reinterpret_cast<std::uint16_t*>(nanTile[1].second.data())[0] = 0x7E00;  // NaN
    expectError(nanTile, "NaN in a tile");
    auto overTile = goodEntries;
    auto* halfs = reinterpret_cast<std::uint16_t*>(overTile[1].second.data());
    halfs[0] = core::floatToHalf(0.9f);  // r > a = 0.25
    expectError(overTile, "colour above alpha");
    auto padded = goodEntries;
    // Tile (2, 1) of a 300 × 200 document: column 44 is outside the document and must be zero.
    reinterpret_cast<std::uint16_t*>(padded[2].second.data())[44 * 4 + 3] = core::floatToHalf(1.0f);
    expectError(padded, "non-zero padding");

    // §12.1: a corrupted CRC.
    writeZip(path, goodEntries);
    std::vector<std::byte> file = fileBytes(path);
    const std::size_t tileData = file.size() / 2;
    file[tileData] ^= std::byte{0x01};
    writeFile(path, file);
    const project::LoadResult corrupted = project::loadProject(path);
    CHECK(!corrupted.snapshot);
    CHECK(corrupted.error.find("CRC") != std::string::npos);
}

// §12.1: interrupting the save before the replace leaves the last complete version readable.
TEST_CASE(failedSaveKeepsPreviousVersion) {
    const fs::path path = testDirectory() / "atomic.rawproj";
    project::ProjectSnapshot v1 = project::emptyProject(256, 256);
    CHECK(project::saveProject(path, v1).ok);
    const auto firstIdentity = project::identifyFile(path);

    project::ProjectSnapshot v2 = sampleProject();
    for (const project::SaveStage stage :
         {project::SaveStage::Written, project::SaveStage::Verified, project::SaveStage::BeforeReplace}) {
        const project::SaveResult failed =
            project::saveProject(path, v2, [stage](project::SaveStage s) { return s == stage; });
        CHECK(!failed.ok && !failed.error.empty());
        const project::LoadResult still = project::loadProject(path);
        CHECK(still.snapshot && sameSnapshot(*still.snapshot, v1));
        CHECK(leftoverFiles(testDirectory(), L"atomic.rawproj.tmp-") == 0);
    }
    CHECK(project::identifyFile(path) == firstIdentity);

    const project::SaveResult replacedSave = project::saveProject(path, v2);
    CHECK(replacedSave.ok);
    const project::LoadResult now = project::loadProject(path);
    CHECK(now.snapshot && sameSnapshot(*now.snapshot, v2));
    CHECK(leftoverFiles(testDirectory(), L"atomic.rawproj.bak") == 0);
    CHECK(project::identifyFile(path) != firstIdentity);
}

// A layer tree with groups, clips and every property survives save and open (§7, §10.3).
TEST_CASE(layerTreeRoundTrip) {
    core::LayerTree tree = core::LayerTree::single();
    const std::uint64_t g = tree.addGroup("Персонаж");
    const std::uint64_t lines = tree.addRaster("Линии");
    tree.move(lines, g, 0);
    const std::uint64_t colour = tree.addRaster("Цвет");
    tree.move(colour, g, 0);
    core::LayerProps p = tree.find(lines)->props;
    p.clipToBase = true;
    p.blendId = 1;
    p.opacity = 0.5;
    p.alphaLock = true;
    tree.setProps(lines, p);
    p = tree.find(g)->props;
    p.visible = false;
    p.locked = true;
    p.blendId = 25;
    tree.setProps(g, p);
    tree.setActive(colour);

    project::ProjectSnapshot s = project::emptyProject(300, 200);
    project::setLayerTree(s, tree);
    CHECK(s.layers.size() == 4 && s.layers[0].id == 1 && s.layers[1].id == g);  // parents first
    CHECK(s.active == colour && s.nextId == tree.nextId());
    auto data = std::make_shared<core::TileData>();
    data->halfs[3] = core::floatToHalf(1.0f);
    s.tiles.push_back({lines, 0, 0, data});
    s.tiles.push_back({colour, 1, 1, data});

    const fs::path path = testDirectory() / "Слои.rawproj";
    CHECK(project::saveProject(path, s).ok);
    const project::LoadResult loaded = project::loadProject(path);
    CHECK(loaded.snapshot.has_value());
    if (!loaded.snapshot) return;
    CHECK(sameSnapshot(s, *loaded.snapshot));
    std::string error;
    const auto back = project::layerTree(*loaded.snapshot, error);
    CHECK(back.has_value());
    if (back) CHECK(*back == tree);
}

// §10.3: the selection is saved as its R8 tiles and opens again; null stays null.
TEST_CASE(selectionRoundTrip) {
    project::ProjectSnapshot s = project::emptyProject(300, 200);
    s.selection = core::rasterizeLasso({{10, 10}, {200.5, 10}, {200.5, 150}, {10, 150}}, 300, 200);
    const fs::path path = testDirectory() / "Выделение.rawproj";
    CHECK(project::saveProject(path, s).ok);
    const project::LoadResult loaded = project::loadProject(path);
    CHECK(loaded.snapshot.has_value());
    if (!loaded.snapshot) {
        std::printf("    load error: %s\n", loaded.error.c_str());
        return;
    }
    CHECK(loaded.snapshot->selection.has_value());
    if (loaded.snapshot->selection) {
        const core::SelectionMask& m = *loaded.snapshot->selection;
        CHECK(m.tiles().size() == s.selection->tiles().size());
        CHECK(m.at(50, 50) == 255 && m.at(200, 50) == 128 && m.at(5, 5) == 0);
    }
    {
        const project::ZipReader zip(path);  // closed again before the next save replaces the file
        CHECK(zip.find("selection/0_0.bin") && zip.find("selection/1_1.bin"));
    }
    // No selection at all.
    s.selection.reset();
    CHECK(project::saveProject(path, s).ok);
    const project::LoadResult none = project::loadProject(path);
    CHECK(none.snapshot && !none.snapshot->selection);
}

// §10.3: vector layers keep their objects in vectors/<layer>.json, profiles as §6.1 JSON.
TEST_CASE(vectorLayerRoundTrip) {
    core::LayerTree tree = core::LayerTree::single();
    const std::uint64_t layer = tree.addVector("Контур");
    core::VectorObject o;
    o.id = tree.reserveId();
    o.version = 3;
    o.rgba = {0.1, 0.2, 0.3, 0.9};
    o.profile = core::builtInBrushes()[2];  // the fountain pen, with pressure curves and tilt
    o.seed = 77;
    o.knots = {core::Knot{tree.reserveId(), {10, 20}, {0, 0}, {5, 1}, 3.5, 3.5, 0.25, 1.0, core::Join::smooth},
               core::Knot{tree.reserveId(), {40, 25}, {-5, -1}, {0, 0}, 1.0, 2.0, -0.5, 0.75, core::Join::corner}};
    project::ProjectSnapshot s = project::emptyProject(300, 200);
    project::setLayerTree(s, tree);
    s.vectors[layer] = {o};
    const fs::path path = testDirectory() / "Вектор.rawproj";
    const project::SaveResult saved = project::saveProject(path, s);
    CHECK(saved.ok);
    if (!saved.ok) std::printf("    save error: %s\n", saved.error.c_str());
    const project::LoadResult loaded = project::loadProject(path);
    CHECK(loaded.snapshot.has_value());
    if (!loaded.snapshot) {
        std::printf("    load error: %s\n", loaded.error.c_str());
        return;
    }
    CHECK(loaded.snapshot->vectors.size() == 1 && loaded.snapshot->vectors.at(layer).size() == 1);
    CHECK(loaded.snapshot->vectors.at(layer)[0] == o);
    std::string error;
    const auto back = project::layerTree(*loaded.snapshot, error);
    CHECK(back && back->find(layer)->kind == core::NodeKind::Vector);
    {
        const project::ZipReader zip(path);
        CHECK(zip.find("vectors/" + std::to_string(layer) + ".json") != nullptr);
    }
    // The whole catalogue survives §6.1 JSON.
    const auto json = project::catalogToJson(core::builtInBrushes());
    const auto brushes = project::catalogFromJson(json::parse(json::serialize(json)));
    CHECK(brushes == core::builtInBrushes());
    // A broken profile is refused with a reason.
    auto bad = project::brushToJson(core::builtInBrushes()[0]);
    bad.set("extra", json::Value::number(1));
    CHECK_THROWS(project::brushFromJson(bad));
}

// ── Brush import and the library (§14) ───────────────────────────────────────

namespace {

const char* kPreset = R"({
  "format":"PresetFormat","version":1,"family":"dip","name":"Перо с мягким входом",
  "sizePx":20,"minSizePct":10,"spacingPct":12.5,"opacityPct":60,"blend":"multiply",
  "smoothing":{"method":"ema","alpha":0.5,"sampleMs":10},
  "fade":{"startPx":2,"endPx":9,"shape":"linear"},
  "sizeCurve":[[0,0],[0.3,0.8],[1,1]],"opacityCurve":[[0,0.3],[1,0.9]]
})";

std::string base64(std::string_view bytes, bool url) {
    const char* table = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char c : bytes) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(table[(acc >> bits) & 63]);
        }
    }
    if (bits > 0) out.push_back(table[(acc << (6 - bits)) & 63]);
    if (!url) {
        while (out.size() % 4) out.push_back('=');
    }
    return out;
}

std::string percent(std::string_view s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

/// An IPBZ record of the observed layout (§14.1), the body as a stored DEFLATE block or raw.
std::vector<std::byte> ipbz(const std::string& name, float diameter, bool deflate, std::size_t cut = 0) {
    std::vector<std::uint8_t> body(198 + name.size() + 4, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) body[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    };
    body[0] = 1, body[1] = 0, body[2] = 2, body[3] = 2;
    const auto len = static_cast<std::uint32_t>(body.size());
    put32(4, len - 12);
    put32(20, std::bit_cast<std::uint32_t>(diameter));
    body[196] = static_cast<std::uint8_t>(name.size() >> 8);
    body[197] = static_cast<std::uint8_t>(name.size() & 0xFF);
    std::memcpy(body.data() + 198, name.data(), name.size());
    put32(len - 4, static_cast<std::uint32_t>(4 - static_cast<std::int64_t>(len)));
    std::vector<std::uint8_t> out = {0x49, 0x50, 0x42, 0x5A, 0, 0x01, 0x38, 0xE4};  // "IPBZ", version 80100
    if (deflate) {
        out.push_back(0x01);  // a final stored block
        out.push_back(static_cast<std::uint8_t>(len & 0xFF));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(~len & 0xFF));
        out.push_back(static_cast<std::uint8_t>((~len >> 8) & 0xFF));
    }
    out.insert(out.end(), body.begin(), body.end() - static_cast<std::ptrdiff_t>(cut));
    std::vector<std::byte> bytes(out.size());
    std::memcpy(bytes.data(), out.data(), out.size());
    return bytes;
}

}  // namespace

TEST_CASE(presetFormatMapping) {
    project::AssetStore assets;
    // §14.4: τ ≈ 14.42695 ms for α = 0.5 at 10 ms; size curve output 0.8 at pressure 0.3; with
    // minSize 0.1 and D = 20 the diameter is 16.4 px.
    CHECK_NEAR(project::emaTau(0.5, 10), 14.42695, 1e-5);
    const auto r = project::importFromPayload(kPreset, assets);
    CHECK(r.profile.has_value() && r.fidelity == "mapped");
    if (!r.profile) {
        std::printf("    error: %s\n", r.error.c_str());
        return;
    }
    const core::BrushProfile& p = *r.profile;
    CHECK(p.id.starts_with("user.") && p.name == "Перо с мягким входом" && p.family == core::BrushFamily::Dip);
    CHECK(p.tip.diameter == 20 && p.ink.spacing == 0.125 && std::abs(p.ink.opacity - 0.6) < 1e-12 && p.ink.blendId == 1);
    CHECK(p.stabilizer.method == core::StabilizerMethod::Ema && std::abs(p.stabilizer.timeMs - 14.42695) < 1e-5);
    CHECK(p.taper.startPx == 2 && p.taper.endPx == 9 && p.taper.shape == core::TaperShape::Linear);
    CHECK_NEAR(core::pressureGain(0.3, p.sizeCurve), 0.8, 1e-12);
    const core::Style style{p.tip.diameter, p.ink.minSize, p.ink.spacing, p.sizeCurve, p.opacityCurve};
    CHECK_NEAR(core::diameter(style, 0.3), 16.4, 1e-9);
    // The same preset through every local transport gives the same profile (§14.4).
    const std::string text = kPreset;
    const std::vector<std::string> forms = {
        "data:application/json," + percent(text),
        "data:application/json;base64," + base64(text, false),
        "base64:" + base64(text, false),
        "base64url:" + base64(text, true),
        "https://example.invalid/brush?preset=" + base64(text, true),
        "https://example.invalid/brush#data=" + percent(text),
        percent(text),
        base64(text, false),
    };
    for (const std::string& f : forms) {
        const auto again = project::importFromPayload(f, assets);
        CHECK(again.profile && again.profile->id == p.id);
    }
    // The canonical §6 JSON of the result imports as itself.
    const auto canonical = project::importFromPayload(json::serialize(project::brushToJson(p)), assets);
    CHECK(canonical.profile && *canonical.profile == p);
}

TEST_CASE(importErrorsKeepTheLibrary) {
    project::AssetStore assets;
    const auto refused = [&](std::string_view text) {
        const auto r = project::importFromPayload(text, assets);
        return !r.profile && !r.error.empty();
    };
    CHECK(refused(R"({"format":"PresetFormat","version":1,"family":"dip","name":"a","sizeCurve":[[0,0],[0.5,0.4],[0.4,0.5],[1,1]]})"));
    CHECK(refused(R"({"format":"PresetFormat","version":1,"family":"dip","name":"a","name":"b"})"));  // repeated key
    CHECK(refused("https://example.invalid/brushes/pen"));                                           // no local data
    CHECK(refused(R"({"format":"PresetFormat","version":1,"family":"dip","name":"a","textureId":"no_such_texture"})"));
    CHECK(refused(R"({"format":"PresetFormat","version":1,"family":"dip","name":"a","glitter":1})"));  // unknown field
    CHECK(refused(R"({"format":"PresetFormat","version":2,"family":"dip","name":"a"})"));
    CHECK(refused(R"({"format":"PresetFormat","version":1,"family":"dip","name":"a","wet":{"bleed":1}})"));
    // IPBZ: the basic record and a cut DEFLATE stream.
    const auto basic = project::importFromBytes(ipbz("Pen (Custom)", 6.0f, true), assets);
    CHECK(basic.profile && basic.fidelity == "basic" && basic.profile->name == "Pen (Custom)" && basic.profile->tip.diameter == 6.0);
    const auto raw = project::importFromBytes(ipbz("Pen (Custom)", 6.0f, false), assets);
    CHECK(raw.profile && raw.profile->id == basic.profile->id);
    CHECK(!project::importFromBytes(ipbz("Pen (Custom)", 6.0f, true, 20), assets).profile);
    // A PNG card is refused with the reason (no local QR decoder in this build).
    const std::vector<std::byte> png = {std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                                        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    CHECK(!project::importFromBytes(png, assets).profile);

    // The library: one entry per ID; a broken file is left as it was.
    const fs::path dir = testDirectory() / "library";
    const auto r = project::importFromPayload(kPreset, assets);
    CHECK(project::saveUserBrush(dir, r).empty());
    CHECK(project::saveUserBrush(dir, project::importFromPayload("base64:" + base64(kPreset, false), assets)).empty());
    std::string error;
    const auto entries = project::loadLibrary(dir, error);
    CHECK(error.empty() && entries.size() == 1 && entries[0].fidelity == "mapped" && entries[0].profile == *r.profile);
    CHECK(fs::exists(dir / "sources" / (entries[0].sourceSha256 + ".bin")));
    writeFile(dir / "user_brushes.json", std::as_bytes(std::span("{broken", 7)));
    CHECK(!project::saveUserBrush(dir, basic).empty());
    const auto after = project::readFileBytes(dir / "user_brushes.json");
    CHECK(after && after->size() == 7);
}

TEST_CASE(masksFromResources) {
    // §14.2 channels: alpha a, redAlpha r·a, inverseLumaAlpha (1 − 0.2126r − 0.7152g − 0.0722b)·a.
    project::ImageRgba image;
    image.width = 2;
    image.height = 1;
    image.rgba = {255, 255, 255, 128, 0, 0, 0, 255};
    const core::MaskImage a = project::maskFrom(image, core::MaskChannel::Alpha);
    const core::MaskImage red = project::maskFrom(image, core::MaskChannel::RedAlpha);
    const core::MaskImage luma = project::maskFrom(image, core::MaskChannel::InverseLumaAlpha);
    CHECK(a.r8[0] == 128 && a.r8[1] == 255 && red.r8[0] == 128 && red.r8[1] == 0 && luma.r8[0] == 0 && luma.r8[1] == 255);
    // The store finds a file by hash and by stem and decodes it through WIC.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    const fs::path dir = testDirectory() / "masks";
    fs::create_directories(dir);
    auto tile = std::make_shared<core::TileData>();
    for (int x = 0; x < 4; ++x) tile->halfs[static_cast<std::size_t>(x) * 4 + 3] = core::floatToHalf(x < 2 ? 1.0f : 0.0f);
    CHECK(project::exportPng(dir / "texture_7.png", 4, 1, [&](int, int) { return tile; }).empty());
    project::AssetStore store;
    store.addDirectory(dir);
    const auto sha = store.byName("texture_7");
    CHECK(sha && core::isSha256(*sha));
    if (sha) {
        const auto mask = store.mask(*sha, core::MaskChannel::Alpha);
        CHECK(mask && mask->width == 4 && mask->r8[0] == 255 && mask->r8[3] == 0);
        // PresetFormat finds it by a number through the local alias texture_<n>.
        const auto r = project::importFromPayload(
            R"({"format":"PresetFormat","version":1,"family":"dip","name":"m","textureId":7,"maskChannel":"alpha"})", store);
        CHECK(r.profile && r.profile->tip.maskId == *sha);
    }
    CoUninitialize();
}

// ── Recovery journal (§10.3) ─────────────────────────────────────────────────

namespace {

/// sampleProject with one tile repainted, one removed and the layer renamed.
project::ProjectSnapshot editedProject(const project::ProjectSnapshot& base) {
    project::ProjectSnapshot s = base;
    auto repainted = std::make_shared<core::TileData>(*s.tiles[0].data);
    setPixel(*repainted, 5, 5, 0.0f, 0.5f, 0.0f, 0.5f);
    s.tiles[0].data = repainted;
    s.tiles.pop_back();
    s.layers.front().name = "После правки";
    return s;
}

std::string toUtf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

}  // namespace

TEST_CASE(journalReplaysCompleteRecords) {
    const fs::path dir = testDirectory() / "journal";
    fs::create_directories(dir);
    const fs::path file = dir / "new.journal";
    const project::ProjectSnapshot a = sampleProject();
    const project::ProjectSnapshot b = editedProject(a);
    std::string error;
    project::JournalHeader header{project::newSessionId(), "Без имени", "", "", 1'700'000'000};
    CHECK(header.session.size() == 36);
    auto journal = project::Journal::create(file, header, error);
    CHECK(journal && error.empty());
    if (!journal) return;
    // A new document: the first record is the full snapshot, the next only what changed.
    const project::JournalState empty;
    const auto [first, afterA] = empty.diff(a);
    CHECK(first.size() == 3);  // manifest and two tiles
    CHECK(journal->append(first).empty());
    const auto [second, afterB] = afterA.diff(b);
    CHECK(second.size() == 3);  // manifest, the repainted tile, the removed one
    CHECK(std::count_if(second.begin(), second.end(), [](const auto& c) { return !c.data; }) == 1);
    CHECK(journal->append(second).empty());
    CHECK(afterB.diff(b).first.empty());
    // A live journal is not taken by another reader.
    CHECK(!project::readJournal(file, error) && error.find("другим") != std::string::npos);
    journal.reset();  // the process ends without a clean exit: the file stays

    auto read = project::readJournal(file, error);
    CHECK(read && read->records.size() == 2 && read->tail.empty() && read->header.session == header.session);
    if (!read) return;
    const project::LoadResult replayed = project::replayJournal(*read);
    CHECK(replayed.snapshot && sameSnapshot(*replayed.snapshot, b));

    // A record cut before its commit marker, or damaged, is not replayed; the prefix is.
    const std::vector<std::byte> bytes = fileBytes(file);
    const fs::path cut = dir / "cut.journal";
    writeFile(cut, std::span(bytes).first(bytes.size() - 3));
    read = project::readJournal(cut, error);
    CHECK(read && read->records.size() == 1 && !read->tail.empty());
    if (read) {
        const project::LoadResult prefix = project::replayJournal(*read);
        CHECK(prefix.snapshot && sameSnapshot(*prefix.snapshot, a));
    }
    std::vector<std::byte> damaged = bytes;
    damaged[damaged.size() - 40] ^= std::byte{0x10};  // inside the second payload
    const fs::path bad = dir / "bad.journal";
    writeFile(bad, damaged);
    read = project::readJournal(bad, error);
    CHECK(read && read->records.size() == 1 && read->tail.find("CRC") != std::string::npos);
    writeFile(bad, std::span(bytes).first(10));
    CHECK(!project::readJournal(bad, error));
}

TEST_CASE(journalOverBaseFile) {
    const fs::path dir = testDirectory() / "journal-base";
    fs::create_directories(dir);
    const fs::path base = dir / "Основа.rawproj";
    const project::ProjectSnapshot a = sampleProject();
    CHECK(project::saveProject(base, a).ok);
    const project::LoadResult opened = project::loadProject(base);
    CHECK(opened.snapshot.has_value());
    if (!opened.snapshot) return;
    std::string error;
    const std::string sha = project::sha256Hex(fileBytes(base));
    project::JournalHeader header{project::newSessionId(), "Основа", toUtf8(base), sha, 0};
    auto journal = project::Journal::create(dir / "base.journal", header, error);
    CHECK(journal != nullptr);
    if (!journal) return;
    // An opened file: nothing is recorded until something changes, then only the change.
    const project::JournalState state = project::JournalState::of(*opened.snapshot);
    CHECK(state.diff(*opened.snapshot).first.empty());
    project::ProjectSnapshot b = *opened.snapshot;
    b.layers.front().visible = false;
    const auto [changes, next] = state.diff(b);
    CHECK(changes.size() == 1 && changes[0].name == "manifest.json");
    CHECK(journal->append(changes).empty());
    journal.reset();
    auto read = project::readJournal(dir / "base.journal", error);
    CHECK(read.has_value());
    if (!read) return;
    const project::LoadResult replayed = project::replayJournal(*read);
    CHECK(replayed.snapshot && sameSnapshot(*replayed.snapshot, b));
    // The base changed since the journal began: refused, nothing is guessed.
    CHECK(project::saveProject(base, editedProject(a)).ok);
    CHECK(!project::replayJournal(*read).snapshot);
    // Removal: closes and deletes.
    auto gone = project::Journal::create(dir / "gone.journal", header, error);
    CHECK(gone && fs::exists(dir / "gone.journal"));
    if (gone) gone->remove();
    CHECK(!fs::exists(dir / "gone.journal"));
}

// ── File exchange (§10.3 "Обмен файлами") ────────────────────────────────────

namespace {

/// A 16 × 8 JPEG, left half red and right half blue, with EXIF orientation `orientation`.
std::vector<std::byte> jpegWithOrientation(std::uint16_t orientation) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &options)) ||
        FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(16, 8))) {
        return {};
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&format);
    ComPtr<IWICMetadataQueryWriter> meta;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(&meta))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = orientation;
        meta->SetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
    }
    std::vector<std::uint8_t> bgr(16 * 8 * 3);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            std::uint8_t* p = &bgr[(static_cast<std::size_t>(y) * 16 + x) * 3];
            p[0] = x < 8 ? 0 : 255;  // B
            p[2] = x < 8 ? 255 : 0;  // R
        }
    }
    if (FAILED(frame->WritePixels(8, 48, static_cast<UINT>(bgr.size()), bgr.data())) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return {};
    }
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    std::vector<std::byte> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    ULONG read = 0;
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
    return out;
}

}  // namespace

TEST_CASE(pictureExchange) {
    // EXIF orientations on a 3 × 2 image with distinct pixels.
    project::ImageRgba image;
    image.width = 3;
    image.height = 2;
    for (int i = 0; i < 6; ++i) image.rgba.insert(image.rgba.end(), {static_cast<std::uint8_t>(i), 0, 0, 255});
    const auto at = [](const project::ImageRgba& im, int x, int y) { return im.rgba[(static_cast<std::size_t>(y) * im.width + x) * 4]; };
    const project::ImageRgba r6 = project::orient(image, 6);  // 90° clockwise: the bottom-left goes top-left
    CHECK(r6.width == 2 && r6.height == 3 && at(r6, 0, 0) == 3 && at(r6, 1, 0) == 0 && at(r6, 1, 2) == 2);
    const project::ImageRgba r8 = project::orient(image, 8);
    CHECK(r8.width == 2 && at(r8, 0, 0) == 2 && at(r8, 0, 2) == 0);
    const project::ImageRgba r3 = project::orient(image, 3);
    CHECK(r3.width == 3 && at(r3, 0, 0) == 5 && at(r3, 2, 1) == 0);
    const project::ImageRgba r5 = project::orient(image, 5);  // transpose
    CHECK(r5.width == 2 && at(r5, 1, 0) == 3 && at(r5, 0, 2) == 2);
    CHECK(project::orient(image, 1).rgba == image.rgba && project::orient(image, 2).rgba[0] == 2);

    // JPEG over the background: a transparent pixel is the background; half-covered red mixes as the
    // canvas does, in encoded sRGB (0.5 → 128).
    std::uint16_t clear[4] = {0, 0, 0, 0};
    std::uint8_t rgb[3];
    project::pmaOverToSrgb8(clear, {1, 1, 1}, rgb);
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);
    const std::uint16_t half[4] = {core::floatToHalf(0.5f), 0, 0, core::floatToHalf(0.5f)};
    project::pmaOverToSrgb8(half, {1, 1, 1}, rgb);
    CHECK(rgb[0] == 255 && rgb[1] == 128 && rgb[2] == 128);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    // PNG in memory decodes back to the same straight pixels.
    const std::vector<std::uint8_t> pixels = {255, 0, 0, 255, 0, 0, 0, 0, 10, 20, 30, 128, 1, 2, 3, 4};
    const std::vector<std::byte> png = project::encodePng(2, 2, pixels);
    std::string error;
    const auto decoded = project::decodeImage(png, error);
    CHECK(!png.empty() && decoded && decoded->rgba == pixels);
    // Pictures: the EXIF orientation is applied (16 × 8 → 8 × 16, red on top for "90° clockwise").
    const std::vector<std::byte> jpeg = jpegWithOrientation(6);
    project::PictureInfo info;
    const auto upright = project::decodePicture(jpeg, error, info);
    CHECK(!jpeg.empty() && upright && info.orientation == 6 && upright->width == 8 && upright->height == 16);
    if (upright) {
        const std::uint8_t* top = &upright->rgba[(2 * 8 + 4) * 4];
        const std::uint8_t* bottom = &upright->rgba[(13 * 8 + 4) * 4];
        CHECK(top[0] > 200 && top[2] < 60 && bottom[2] > 200 && bottom[0] < 60);
    }
    CHECK(!project::decodePicture(std::as_bytes(std::span("not an image", 12)), error, info));

    // JPEG export: composed over white, a transparent document is white.
    const fs::path dir = testDirectory() / "exchange";
    fs::create_directories(dir);
    auto tile = std::make_shared<core::TileData>();
    setPixel(*tile, 0, 0, 0.0f, 0.0f, 0.5f, 0.5f);
    CHECK(project::exportJpeg(dir / "flat.jpg", 64, 32, [&](int, int) { return tile; }, {1, 1, 1}).empty());
    const auto bytes = project::readFileBytes(dir / "flat.jpg");
    const auto flat = bytes ? project::decodeImage(*bytes, error) : std::nullopt;
    CHECK(flat && flat->width == 64 && flat->height == 32);
    if (flat) {
        const std::uint8_t* corner = &flat->rgba[(20 * 64 + 50) * 4];
        CHECK(corner[0] > 245 && corner[1] > 245 && corner[2] > 245 && corner[3] == 255);
    }

    // thumbnail.png travels with the project and does not disturb opening.
    project::ProjectSnapshot s = sampleProject();
    s.thumbnailPng = std::make_shared<const std::vector<std::byte>>(png);
    CHECK(project::saveProject(dir / "thumb.rawproj", s).ok);
    {
        const project::ZipReader zip(dir / "thumb.rawproj");
        CHECK(zip.find("thumbnail.png") != nullptr);
    }
    const project::LoadResult loaded = project::loadProject(dir / "thumb.rawproj");
    CHECK(loaded.snapshot && sameSnapshot(*loaded.snapshot, s));
    CoUninitialize();
}

// Version 0.1.0 stored linear light: such files open converted to the sRGB canvas, tiles and vector
// colours, flagged as migrated; what is written again says "srgb-pma-f16le".
TEST_CASE(linearFilesMigrateToSrgb) {
    project::ProjectSnapshot s = sampleProject();
    auto grey = std::make_shared<core::TileData>();
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) setPixel(*grey, x, y, 0.21404114f, 0.21404114f, 0.21404114f, 1.0f);
    }
    s.tiles = {{1, 0, 0, grey}};
    std::string manifest = project::manifestJson(s);
    CHECK(manifest.find("\"srgb-pma-f16le\"") != std::string::npos);
    manifest.replace(manifest.find("\"srgb-pma-f16le\""), 16, "\"linear-srgb-pma-f16le\"");
    project::Entries entries;
    entries["manifest.json"].assign(reinterpret_cast<const std::byte*>(manifest.data()),
                                    reinterpret_cast<const std::byte*>(manifest.data()) + manifest.size());
    const auto tile = std::as_bytes(std::span(grey->halfs));
    entries["tiles/1/0_0.bin"].assign(tile.begin(), tile.end());
    const project::LoadResult r = project::loadFromEntries(entries);
    CHECK(r.snapshot && r.snapshot->migratedFromLinear && r.snapshot->tiles.size() == 1);
    if (r.snapshot && !r.snapshot->tiles.empty()) {
        const float v = core::halfToFloat(r.snapshot->tiles[0].data->halfs[0]);
        CHECK(std::abs(v - 0.5f) < 0.001f);  // linear 0.214 is sRGB 0.5
        CHECK(core::halfToFloat(r.snapshot->tiles[0].data->halfs[3]) == 1.0f);
        CHECK(project::manifestJson(*r.snapshot).find("\"srgb-pma-f16le\"") != std::string::npos);
    }
    // A current file is not migrated.
    const std::string currentManifest = project::manifestJson(s);
    entries["manifest.json"].assign(reinterpret_cast<const std::byte*>(currentManifest.data()),
                                    reinterpret_cast<const std::byte*>(currentManifest.data()) + currentManifest.size());
    const project::LoadResult current = project::loadFromEntries(entries);
    CHECK(current.snapshot && !current.snapshot->migratedFromLinear &&
          core::halfToFloat(current.snapshot->tiles[0].data->halfs[0]) == core::halfToFloat(grey->halfs[0]));
}

TEST_CASE(pngExportStraightAlpha) {
    std::uint8_t out[4];
    const std::uint16_t transparent[4] = {core::floatToHalf(0.3f), 0, 0, 0};
    project::pmaToStraightSrgb8(transparent, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);  // rgb is zero where alpha is zero
    const std::uint16_t halfRed[4] = {core::floatToHalf(0.5f), 0, 0, core::floatToHalf(0.5f)};
    project::pmaToStraightSrgb8(halfRed, out);
    CHECK(out[0] == 255 && out[1] == 0 && out[3] == 128);
    const std::uint16_t grey[4] = {core::floatToHalf(0.25f), core::floatToHalf(0.25f), core::floatToHalf(0.25f),
                                   core::floatToHalf(0.5f)};
    project::pmaToStraightSrgb8(grey, out);
    CHECK(out[0] == 128 && out[3] == 128);  // the canvas value straight: 0.25 / 0.5

    // Through WIC and back: 300 × 200 with the sample tiles.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    const project::ProjectSnapshot s = sampleProject();
    const fs::path path = testDirectory() / "export.png";
    const std::string error = project::exportPng(path, s.width, s.height, [&](int tx, int ty) {
        for (const auto& t : s.tiles) {
            if (t.tx == tx && t.ty == ty) return t.data;
        }
        return std::shared_ptr<const core::TileData>{};
    });
    CHECK(error.empty());
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    bool decoded = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                   SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &decoder)) &&
                   SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                   SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                                   nullptr, 0.0, WICBitmapPaletteTypeCustom));
    CHECK(decoded);
    if (decoded) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        CHECK(w == 300 && h == 200);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
        CHECK(SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())));
        const auto at = [&](int x, int y) { return &pixels[(static_cast<std::size_t>(y) * w + x) * 4]; };
        CHECK(at(200, 10)[3] == 0 && at(200, 10)[0] == 0);  // empty tile: transparent, rgb 0
        CHECK(at(256 + 10, 128 + 10)[3] == 128);             // corner tile: a = 0.5
        CHECK(at(256 + 10, 128 + 10)[0] == 255);             // straight red 1.0
        CHECK(at(299, 199)[3] == 128);                       // last document pixel
    }
    frame.Reset();
    converter.Reset();
    decoder.Reset();
    factory.Reset();
    CoUninitialize();
}

int main() {
    const int result = test::runAll();
    std::error_code ec;
    fs::remove_all(testDirectory(), ec);
    return result;
}

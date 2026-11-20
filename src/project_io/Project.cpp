#include "project_io/Project.hpp"

#include "core/Color.hpp"
#include "core/Half.hpp"

#include "project_io/BrushJson.hpp"
#include "project_io/Json.hpp"
#include "project_io/Zip.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace chroma::project {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr int kMajor = 1;
constexpr int kMinor = 0;
constexpr std::string_view kFormat = "chroma.rawproj";
constexpr std::string_view kColor = "srgb-pma-f16le";
constexpr std::string_view kLegacyLinearColor = "linear-srgb-pma-f16le";  // version 0.1.0
constexpr std::size_t kMaxLayers = 4096;  // spec §10.3
constexpr int kMaxDepth = 64;
constexpr std::uint64_t kMaxManifestBytes = std::uint64_t{256} << 20;
// Features a file may require (requiredFeatures, spec §14). brushDynamics1 concerns vector profiles,
// which this version rejects separately.
constexpr std::string_view kKnownFeatures[] = {"brushDynamics1"};

[[noreturn]] void fail(std::string what) { throw std::runtime_error(std::move(what)); }

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Decimal, no sign, no leading zeros.
std::optional<std::uint64_t> parseDecimal(std::string_view s) {
    if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0')) return std::nullopt;
    if (!std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
    return v;
}

// Object IDs are positive uint64 written as decimal strings (spec §2).
std::uint64_t parseId(const Value& v, std::string_view what) {
    if (!v.isString()) fail(std::format("manifest.json: {} должен быть строкой с числом", what));
    const auto id = parseDecimal(v.asString());
    if (!id || *id == 0) fail(std::format("manifest.json: недопустимый {}: «{}»", what, v.asString()));
    return *id;
}

int tilesAcross(int pixels) { return (pixels + core::kTileSize - 1) / core::kTileSize; }

std::string tilePath(std::uint64_t layer, int tx, int ty) { return std::format("tiles/{}/{}_{}.bin", layer, tx, ty); }

std::string vectorPath(std::uint64_t layer) { return std::format("vectors/{}.json", layer); }

// ── Vector objects as JSON (§10.3) ──

Value pairOf(core::V v) {
    Value a = Value::array();
    a.push(Value::number(v.x));
    a.push(Value::number(v.y));
    return a;
}

core::V pairFrom(const Value* v, std::string_view what) {
    if (!v || !v->isArray() || v->items().size() != 2 || !v->items()[0].isNumber() || !v->items()[1].isNumber()) {
        fail(std::format("векторный узел: {} должен быть парой [x, y]", what));
    }
    return {v->items()[0].asNumber(), v->items()[1].asNumber()};
}

constexpr std::string_view kJoins[] = {"corner", "smooth", "symmetric"};

Value objectToJson(const core::VectorObject& o) {
    Value v = Value::object();
    v.set("id", Value::string(std::to_string(o.id)));
    v.set("version", Value::number(static_cast<double>(o.version)));
    Value rgba = Value::array();
    for (const double c : o.rgba) rgba.push(Value::number(c));
    v.set("rgba", std::move(rgba));
    v.set("profile", brushToJson(o.profile));
    v.set("seed", Value::number(o.seed));
    Value& nodes = v.set("nodes", Value::array());
    for (const core::Knot& k : o.knots) {
        Value n = Value::object();
        n.set("id", Value::string(std::to_string(k.id)));
        n.set("p", pairOf(k.pos));
        n.set("in", pairOf(k.in));
        n.set("out", pairOf(k.out));
        n.set("width", Value::number(k.width));
        n.set("spacingDiameter", Value::number(k.spacingDiameter));
        n.set("angle", Value::number(k.angle));
        n.set("opacity", Value::number(k.opacity));
        n.set("join", Value::string(std::string(kJoins[static_cast<int>(k.join)])));
        nodes.push(std::move(n));
    }
    return v;
}

core::VectorObject objectFromJson(const Value& v) {
    if (!v.isObject()) fail("векторный объект должен быть объектом JSON");
    for (const std::string& key : v.keys()) {
        if (key != "id" && key != "version" && key != "rgba" && key != "profile" && key != "seed" && key != "nodes") {
            fail("лишнее поле векторного объекта: " + key);
        }
    }
    const auto field = [&](std::string_view key) -> const Value& {
        const Value* f = v.find(key);
        if (!f) fail(std::format("у векторного объекта нет поля {}", key));
        return *f;
    };
    core::VectorObject o;
    o.id = parseId(field("id"), "id объекта");
    const auto version = field("version").asInteger(1, std::numeric_limits<std::int64_t>::max());
    if (!version) fail("version объекта — целое ≥ 1");
    o.version = static_cast<std::uint64_t>(*version);
    const Value& rgba = field("rgba");
    if (!rgba.isArray() || rgba.items().size() != 4) fail("rgba объекта — 4 числа");
    for (std::size_t i = 0; i < 4; ++i) {
        if (!rgba.items()[i].isNumber()) fail("rgba объекта — 4 числа");
        o.rgba[i] = rgba.items()[i].asNumber();
    }
    o.profile = brushFromJson(field("profile"));
    const auto seed = field("seed").asInteger(0, 4294967295LL);
    if (!seed) fail("seed объекта — целое от 0 до 4294967295");
    o.seed = static_cast<std::uint32_t>(*seed);
    const Value& nodes = field("nodes");
    if (!nodes.isArray()) fail("nodes объекта должен быть массивом");
    for (const Value& n : nodes.items()) {
        if (!n.isObject()) fail("векторный узел должен быть объектом");
        core::Knot k{};
        const Value* id = n.find("id");
        if (!id) fail("у векторного узла нет id");
        k.id = parseId(*id, "id узла");
        k.pos = pairFrom(n.find("p"), "p");
        k.in = pairFrom(n.find("in"), "in");
        k.out = pairFrom(n.find("out"), "out");
        const auto num = [&](std::string_view key) {
            const Value* f = n.find(key);
            if (!f || !f->isNumber()) fail(std::format("векторный узел: {} должен быть числом", key));
            return f->asNumber();
        };
        k.width = num("width");
        k.spacingDiameter = num("spacingDiameter");
        k.angle = num("angle");
        k.opacity = num("opacity");
        const Value* join = n.find("join");
        const auto it = join && join->isString() ? std::find(std::begin(kJoins), std::end(kJoins), join->asString()) : std::end(kJoins);
        if (it == std::end(kJoins)) fail("векторный узел: join — corner, smooth или symmetric");
        k.join = static_cast<core::Join>(it - std::begin(kJoins));
        if (n.keys().size() != 9) fail("векторный узел: лишние или недостающие поля");
        o.knots.push_back(k);
    }
    if (const std::string problem = core::validateObject(o); !problem.empty()) fail("векторный объект: " + problem);
    return o;
}

std::string_view typeName(LayerType t) {
    switch (t) {
    case LayerType::Raster: return "raster";
    case LayerType::Vector: return "vector";
    case LayerType::Group: return "group";
    }
    return "raster";
}

// Kinds of archive entries allowed by the §10.3 table.
enum class EntryKind : std::uint8_t { Manifest, Thumbnail, Tile, Vector, Asset, Selection, Unknown };

struct TilePathParts {
    std::uint64_t layer = 0;
    std::uint64_t tx = 0;
    std::uint64_t ty = 0;
};

bool parseTileCoords(std::string_view s, std::uint64_t& x, std::uint64_t& y) {
    const std::size_t underscore = s.find('_');
    if (underscore == std::string_view::npos) return false;
    const auto a = parseDecimal(s.substr(0, underscore));
    const auto b = parseDecimal(s.substr(underscore + 1));
    if (!a || !b) return false;
    x = *a;
    y = *b;
    return true;
}

EntryKind classify(std::string_view name, TilePathParts& tile) {
    if (name == "manifest.json") return EntryKind::Manifest;
    if (name == "thumbnail.png") return EntryKind::Thumbnail;
    const auto stripSuffix = [](std::string_view s, std::string_view suffix, std::string_view& out) {
        if (s.size() <= suffix.size() || s.substr(s.size() - suffix.size()) != suffix) return false;
        out = s.substr(0, s.size() - suffix.size());
        return true;
    };
    std::string_view body;
    if (name.starts_with("tiles/") && stripSuffix(name.substr(6), ".bin", body)) {
        const std::size_t slash = body.find('/');
        if (slash == std::string_view::npos) return EntryKind::Unknown;
        const auto layer = parseDecimal(body.substr(0, slash));
        if (!layer || *layer == 0 || !parseTileCoords(body.substr(slash + 1), tile.tx, tile.ty)) return EntryKind::Unknown;
        tile.layer = *layer;
        return EntryKind::Tile;
    }
    if (name.starts_with("vectors/") && stripSuffix(name.substr(8), ".json", body)) {
        return parseDecimal(body) ? EntryKind::Vector : EntryKind::Unknown;
    }
    if (name.starts_with("assets/") && stripSuffix(name.substr(7), ".png", body)) {
        const bool hash = body.size() == 64 && std::all_of(body.begin(), body.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
        return hash ? EntryKind::Asset : EntryKind::Unknown;
    }
    if (name.starts_with("selection/") && stripSuffix(name.substr(10), ".bin", body)) {
        std::uint64_t x = 0, y = 0;
        return parseTileCoords(body, x, y) ? EntryKind::Selection : EntryKind::Unknown;
    }
    return EntryKind::Unknown;
}

struct TileRef {
    std::uint64_t layer;
    int tx;
    int ty;
};

// Parses and validates manifest.json; returns the snapshot without tile data, the tile list and the
// selection's tile list (nullopt: no selection).
ProjectSnapshot parseManifest(const Value& m, std::vector<TileRef>& tiles,
                              std::optional<std::vector<std::pair<int, int>>>& selectionTiles) {
    if (!m.isObject()) fail("manifest.json: ожидался объект");
    const auto field = [&](std::string_view key) -> const Value& {
        const Value* v = m.find(key);
        if (!v) fail(std::format("manifest.json: нет поля «{}»", key));
        return *v;
    };
    const auto integer = [&](std::string_view key, std::int64_t lo, std::int64_t hi) {
        const auto v = field(key).asInteger(lo, hi);
        if (!v) fail(std::format("manifest.json: поле «{}» должно быть целым числом от {} до {}", key, lo, hi));
        return *v;
    };

    if (!field("format").isString() || field("format").asString() != kFormat) fail("это не проект ChromaStudio");
    const auto major = field("major").asInteger(0, 1'000'000);
    if (!major) fail("manifest.json: недопустимая версия формата");
    if (*major != kMajor) fail(std::format("версия формата {} не поддерживается (нужна {})", *major, kMajor));
    integer("minor", 0, 1'000'000);
    if (const Value* required = m.find("requiredFeatures")) {
        if (!required->isArray()) fail("manifest.json: requiredFeatures должен быть массивом");
        for (const Value& f : required->items()) {
            if (!f.isString()) fail("manifest.json: requiredFeatures должен содержать строки");
            const bool known = std::find(std::begin(kKnownFeatures), std::end(kKnownFeatures), f.asString()) !=
                               std::end(kKnownFeatures);
            if (!known) fail(std::format("файлу нужна возможность «{}», которой нет в этой версии", f.asString()));
        }
    }

    ProjectSnapshot s;
    s.width = static_cast<int>(integer("width", 1, 32768));
    s.height = static_cast<int>(integer("height", 1, 32768));
    s.dpi = static_cast<int>(integer("dpi", 1, 2400));
    if (!field("color").isString() ||
        (field("color").asString() != kColor && field("color").asString() != kLegacyLinearColor)) {
        fail("manifest.json: неизвестное цветовое пространство");
    }
    s.migratedFromLinear = field("color").asString() == kLegacyLinearColor;
    if (integer("tileSize", 1, 1 << 16) != core::kTileSize) fail("manifest.json: поддерживается только tileSize 128");
    s.nextId = parseId(field("nextId"), "nextId");
    s.active = parseId(field("active"), "active");

    // Layer tree.
    const Value& layers = field("layers");
    if (!layers.isArray() || layers.items().empty() || layers.items().size() > kMaxLayers) {
        fail("manifest.json: layers должен содержать от 1 до 4096 узлов");
    }
    std::map<std::uint64_t, std::size_t> byId;
    for (const Value& node : layers.items()) {
        if (!node.isObject()) fail("manifest.json: слой должен быть объектом");
        const auto member = [&](std::string_view key) -> const Value& {
            const Value* v = node.find(key);
            if (!v) fail(std::format("manifest.json: у слоя нет поля «{}»", key));
            return *v;
        };
        const auto flag = [&](std::string_view key) {
            const Value& v = member(key);
            if (!v.isBool()) fail(std::format("manifest.json: поле слоя «{}» должно быть true или false", key));
            return v.asBool();
        };
        LayerInfo layer;
        layer.id = parseId(member("id"), "id слоя");
        const Value& parent = member("parent");
        if (!parent.isNull()) layer.parent = parseId(parent, "parent");
        const Value& type = member("type");
        if (!type.isString()) fail("manifest.json: type слоя должен быть строкой");
        if (type.asString() == "raster") {
            layer.type = LayerType::Raster;
        } else if (type.asString() == "vector") {
            layer.type = LayerType::Vector;
        } else if (type.asString() == "group") {
            layer.type = LayerType::Group;
        } else {
            fail(std::format("manifest.json: неизвестный тип слоя «{}»", type.asString()));
        }
        const Value& name = member("name");
        if (!name.isString() || name.asString().empty() || name.asString().size() > 1024) {
            fail("manifest.json: недопустимое имя слоя");
        }
        layer.name = name.asString();
        layer.visible = flag("visible");
        const Value& opacity = member("opacity");
        if (!opacity.isNumber() || !(opacity.asNumber() >= 0.0 && opacity.asNumber() <= 1.0)) {
            fail("manifest.json: opacity слоя должна быть от 0 до 1");
        }
        layer.opacity = opacity.asNumber();
        const auto blend = member("blendId").asInteger(0, 26);
        if (!blend) fail("manifest.json: blendId слоя должен быть от 0 до 26");
        layer.blendId = static_cast<int>(*blend);
        layer.clipToBase = flag("clipToBase");
        layer.alphaLock = flag("alphaLock");
        layer.locked = flag("locked");
        if (!byId.emplace(layer.id, s.layers.size()).second) fail(std::format("manifest.json: повтор id слоя {}", layer.id));
        s.layers.push_back(std::move(layer));
    }
    std::uint64_t maxId = 0;
    for (const LayerInfo& layer : s.layers) {
        maxId = std::max(maxId, layer.id);
        // Parents resolve after every node is known: a group, no cycles, depth ≤ 64.
        int depth = 1;
        for (std::optional<std::uint64_t> p = layer.parent; p; ++depth) {
            const auto it = byId.find(*p);
            if (it == byId.end()) fail(std::format("manifest.json: родитель {} не найден", *p));
            const LayerInfo& parentLayer = s.layers[it->second];
            if (parentLayer.type != LayerType::Group) fail("manifest.json: родителем может быть только группа");
            if (depth > kMaxDepth) fail("manifest.json: дерево слоёв глубже 64 уровней или содержит цикл");
            p = parentLayer.parent;
        }
    }
    if (s.nextId <= maxId) fail("manifest.json: nextId должен быть больше всех id");
    if (!byId.contains(s.active)) fail("manifest.json: активный слой не найден");

    // Tile list.
    const Value& tileList = field("tiles");
    if (!tileList.isArray()) fail("manifest.json: tiles должен быть массивом");
    const int maxTx = tilesAcross(s.width);
    const int maxTy = tilesAcross(s.height);
    std::set<std::tuple<std::uint64_t, int, int>> seen;
    for (const Value& t : tileList.items()) {
        if (!t.isObject() || !t.find("layer") || !t.find("tx") || !t.find("ty")) {
            fail("manifest.json: запись тайла должна содержать layer, tx, ty");
        }
        const std::uint64_t layer = parseId(*t.find("layer"), "layer тайла");
        const auto tx = t.find("tx")->asInteger(0, maxTx - 1);
        const auto ty = t.find("ty")->asInteger(0, maxTy - 1);
        if (!tx || !ty) fail("manifest.json: координаты тайла вне сетки документа");
        const auto it = byId.find(layer);
        if (it == byId.end() || s.layers[it->second].type != LayerType::Raster) {
            fail(std::format("manifest.json: тайл ссылается на {}, а это не растровый слой", layer));
        }
        if (!seen.emplace(layer, static_cast<int>(*tx), static_cast<int>(*ty)).second) fail("manifest.json: повтор тайла");
        tiles.push_back({layer, static_cast<int>(*tx), static_cast<int>(*ty)});
    }

    // §10.3: vectors lists the IDs of every vector layer, each with its vectors/<layer>.json.
    const Value& vectors = field("vectors");
    if (!vectors.isArray()) fail("manifest.json: vectors должен быть массивом");
    std::set<std::uint64_t> listedVectors;
    for (const Value& id : vectors.items()) {
        const std::uint64_t layer = parseId(id, "id векторного слоя");
        const auto it = byId.find(layer);
        if (it == byId.end() || s.layers[it->second].type != LayerType::Vector) {
            fail(std::format("manifest.json: vectors ссылается на {}, а это не векторный слой", layer));
        }
        if (!listedVectors.insert(layer).second) fail("manifest.json: повтор в vectors");
        s.vectors[layer];  // the objects are read from the archive
    }
    for (const LayerInfo& l : s.layers) {
        if (l.type == LayerType::Vector && !listedVectors.contains(l.id)) {
            fail(std::format("manifest.json: векторный слой {} не указан в vectors", l.id));
        }
    }
    const Value& assets = field("assets");
    if (!assets.isArray()) fail("manifest.json: assets должен быть массивом");
    if (!assets.items().empty()) fail("пользовательские текстуры в проекте пока не поддерживаются этой версией");
    // §10.3: null means no active selection; otherwise {"tiles": [[tx, ty], ...]} lists the R8 tiles
    // that exist (an empty list is an empty mask).
    const Value& selection = field("selection");
    if (!selection.isNull()) {
        const Value* list = selection.isObject() ? selection.find("tiles") : nullptr;
        if (!list || !list->isArray()) fail("manifest.json: selection должен быть null или объектом с tiles");
        std::set<std::pair<int, int>> unique;
        selectionTiles.emplace();
        for (const Value& pair : list->items()) {
            if (!pair.isArray() || pair.items().size() != 2) fail("manifest.json: тайл выделения — пара [tx, ty]");
            const auto tx = pair.items()[0].asInteger(0, maxTx - 1);
            const auto ty = pair.items()[1].asInteger(0, maxTy - 1);
            if (!tx || !ty) fail("manifest.json: тайл выделения вне сетки документа");
            if (!unique.emplace(static_cast<int>(*tx), static_cast<int>(*ty)).second) fail("manifest.json: повтор тайла выделения");
            selectionTiles->emplace_back(static_cast<int>(*tx), static_cast<int>(*ty));
        }
    }
    return s;
}

std::string randomSuffix() {
    std::random_device device;
    return std::format("{:08x}", device());
}

}  // namespace

ProjectSnapshot emptyProject(int width, int height) {
    ProjectSnapshot s;
    s.width = width;
    s.height = height;
    s.layers.push_back(LayerInfo{});
    return s;
}

std::optional<core::LayerTree> layerTree(const ProjectSnapshot& snapshot, std::string& error) {
    std::vector<core::LayerTree::Entry> entries;
    entries.reserve(snapshot.layers.size());
    for (const LayerInfo& l : snapshot.layers) {
        core::LayerTree::Entry e;
        e.id = l.id;
        e.parent = l.parent.value_or(0);
        e.kind = l.type == LayerType::Group    ? core::NodeKind::Group
                 : l.type == LayerType::Vector ? core::NodeKind::Vector
                                               : core::NodeKind::Raster;
        e.props.name = l.name;
        e.props.visible = l.visible;
        e.props.opacity = l.opacity;
        e.props.blendId = l.blendId;
        e.props.clipToBase = l.clipToBase;
        e.props.alphaLock = l.alphaLock;
        e.props.locked = l.locked;
        entries.push_back(std::move(e));
    }
    return core::LayerTree::fromEntries(entries, snapshot.active, snapshot.nextId, error);
}

void setLayerTree(ProjectSnapshot& snapshot, const core::LayerTree& tree) {
    snapshot.layers.clear();
    for (const std::uint64_t id : tree.preorder()) {
        const core::LayerNode& n = *tree.find(id);
        LayerInfo l;
        l.id = n.id;
        if (n.parent != 0) l.parent = n.parent;
        l.type = n.kind == core::NodeKind::Group    ? LayerType::Group
                 : n.kind == core::NodeKind::Vector ? LayerType::Vector
                                                    : LayerType::Raster;
        l.name = n.props.name;
        l.visible = n.props.visible;
        l.opacity = n.props.opacity;
        l.blendId = n.props.blendId;
        l.clipToBase = n.props.clipToBase;
        l.alphaLock = n.props.alphaLock;
        l.locked = n.props.locked;
        snapshot.layers.push_back(std::move(l));
    }
    snapshot.active = tree.active();
    snapshot.nextId = tree.nextId();
}

std::string manifestJson(const ProjectSnapshot& s) {
    Value m = Value::object();
    m.set("format", Value::string(std::string(kFormat)));
    m.set("major", Value::number(kMajor));
    m.set("minor", Value::number(kMinor));
    m.set("width", Value::number(s.width));
    m.set("height", Value::number(s.height));
    m.set("dpi", Value::number(s.dpi));
    m.set("color", Value::string(std::string(kColor)));
    m.set("tileSize", Value::number(core::kTileSize));
    m.set("nextId", Value::string(std::to_string(s.nextId)));
    m.set("active", Value::string(std::to_string(s.active)));
    Value& layers = m.set("layers", Value::array());
    for (const LayerInfo& l : s.layers) {
        Value node = Value::object();
        node.set("id", Value::string(std::to_string(l.id)));
        node.set("parent", l.parent ? Value::string(std::to_string(*l.parent)) : Value::null());
        node.set("type", Value::string(std::string(typeName(l.type))));
        node.set("name", Value::string(l.name));
        node.set("visible", Value::boolean(l.visible));
        node.set("opacity", Value::number(l.opacity));
        node.set("blendId", Value::number(l.blendId));
        node.set("clipToBase", Value::boolean(l.clipToBase));
        node.set("alphaLock", Value::boolean(l.alphaLock));
        node.set("locked", Value::boolean(l.locked));
        layers.push(std::move(node));
    }
    std::vector<const TileEntry*> ordered;
    for (const TileEntry& t : s.tiles) ordered.push_back(&t);
    std::sort(ordered.begin(), ordered.end(), [](const TileEntry* a, const TileEntry* b) {
        return std::tie(a->layer, a->ty, a->tx) < std::tie(b->layer, b->ty, b->tx);
    });
    Value& tiles = m.set("tiles", Value::array());
    for (const TileEntry* t : ordered) {
        Value entry = Value::object();
        entry.set("layer", Value::string(std::to_string(t->layer)));
        entry.set("tx", Value::number(t->tx));
        entry.set("ty", Value::number(t->ty));
        tiles.push(std::move(entry));
    }
    Value& vectorList = m.set("vectors", Value::array());
    for (const LayerInfo& l : s.layers) {
        if (l.type == LayerType::Vector) vectorList.push(Value::string(std::to_string(l.id)));
    }
    m.set("assets", Value::array());
    if (s.selection) {
        Value selection = Value::object();
        Value& list = selection.set("tiles", Value::array());
        for (const auto& [key, tile] : s.selection->tiles()) {
            Value pair = Value::array();
            pair.push(Value::number(key.first));
            pair.push(Value::number(key.second));
            list.push(std::move(pair));
        }
        m.set("selection", std::move(selection));
    } else {
        m.set("selection", Value::null());
    }
    return json::serialize(m);
}

std::optional<FileIdentity> identifyFile(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    if (!ok) return std::nullopt;
    FileIdentity id;
    id.volume = info.dwVolumeSerialNumber;
    id.fileIndex = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    id.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    id.lastWrite = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                   info.ftLastWriteTime.dwLowDateTime;
    return id;
}

// The §10.3 checks over records by logical name: a ZIP file or the entries of a recovery journal.
struct EntryAccess {
    std::function<std::vector<std::pair<std::string, std::uint64_t>>()> list;  // name, uncompressed size
    std::function<std::optional<std::uint64_t>(const std::string&)> size;
    std::function<std::vector<std::byte>(const std::string&)> read;
};

static ProjectSnapshot loadFrom(const EntryAccess& src) {
    {

        // Every entry must be one of the §10.3 records.
        std::set<std::tuple<std::uint64_t, int, int>> tileEntries;
        std::size_t selectionEntries = 0;
        std::set<std::uint64_t> vectorEntries;
        for (const auto& [entryName, entrySize] : src.list()) {
            TilePathParts parts;
            switch (classify(entryName, parts)) {
            case EntryKind::Manifest:
            case EntryKind::Thumbnail: break;
            case EntryKind::Tile:
                if (parts.tx > 32768 || parts.ty > 32768) fail("недопустимые координаты тайла: " + entryName);
                tileEntries.emplace(parts.layer, static_cast<int>(parts.tx), static_cast<int>(parts.ty));
                break;
            case EntryKind::Vector: {
                std::string_view body = entryName;
                body.remove_prefix(8);
                body.remove_suffix(5);
                vectorEntries.insert(*parseDecimal(body));
                break;
            }
            case EntryKind::Asset: fail("пользовательские текстуры в проекте пока не поддерживаются этой версией");
            case EntryKind::Selection: ++selectionEntries; break;
            case EntryKind::Unknown: fail("недопустимая запись в проекте: " + entryName);
            }
        }

        const std::optional<std::uint64_t> manifestSize = src.size("manifest.json");
        if (!manifestSize) fail("в файле нет manifest.json");
        if (*manifestSize > kMaxManifestBytes) fail("manifest.json слишком велик");
        const std::vector<std::byte> manifestBytes = src.read("manifest.json");
        Value manifest;
        try {
            manifest = json::parse(std::string_view(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()));
        } catch (const std::exception& error) {
            fail(std::string("manifest.json повреждён: ") + error.what());
        }
        std::vector<TileRef> listed;
        std::optional<std::vector<std::pair<int, int>>> selectionTiles;
        ProjectSnapshot snapshot = parseManifest(manifest, listed, selectionTiles);
        // Vector layers: one JSON array of objects each (§10.3); IDs unique and below nextId.
        if (vectorEntries.size() != snapshot.vectors.size()) fail("список векторных слоёв не совпадает с содержимым архива");
        std::set<std::uint64_t> ids;
        for (const LayerInfo& l : snapshot.layers) ids.insert(l.id);
        for (auto& [layer, objects] : snapshot.vectors) {
            if (!vectorEntries.contains(layer)) fail("нет записи " + vectorPath(layer));
            const std::optional<std::uint64_t> entry = src.size(vectorPath(layer));
            if (!entry || *entry > kMaxManifestBytes * 64) fail(vectorPath(layer) + " слишком велик");
            const std::vector<std::byte> bytes = src.read(vectorPath(layer));
            Value list;
            try {
                list = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            } catch (const std::exception& error) {
                fail(vectorPath(layer) + " повреждён: " + error.what());
            }
            if (!list.isArray()) fail(vectorPath(layer) + ": ожидался массив объектов");
            for (const Value& item : list.items()) {
                core::VectorObject o = objectFromJson(item);
                if (!ids.insert(o.id).second) fail(std::format("повтор id {} в векторных объектах", o.id));
                for (const core::Knot& k : o.knots) {
                    if (!ids.insert(k.id).second) fail(std::format("повтор id узла {}", k.id));
                }
                objects.push_back(std::move(o));
            }
        }
        if (!ids.empty() && *ids.rbegin() >= snapshot.nextId) fail("nextId должен быть больше всех id, включая векторные");
        if (selectionEntries != (selectionTiles ? selectionTiles->size() : 0)) {
            fail("список тайлов выделения в manifest.json не совпадает с содержимым архива");
        }
        if (selectionTiles) {
            core::SelectionMask mask;
            for (const auto& [tx, ty] : *selectionTiles) {
                const std::string name = std::format("selection/{}_{}.bin", tx, ty);
                const std::optional<std::uint64_t> entry = src.size(name);
                if (!entry) fail("нет записи " + name);
                if (*entry != core::kMaskTileBytes) fail(std::format("{}: размер не {} байт", name, core::kMaskTileBytes));
                const std::vector<std::byte> bytes = src.read(name);
                auto tile = std::make_shared<core::MaskTile>();
                std::memcpy(tile->bytes.data(), bytes.data(), core::kMaskTileBytes);
                // Outside the document the mask is zero, like the tiles.
                const core::TileExtent e = core::tileExtent(snapshot.width, snapshot.height, tx, ty);
                for (int y = 0; y < core::kTileSize; ++y) {
                    for (int x = 0; x < core::kTileSize; ++x) {
                        if ((x >= e.validWidth || y >= e.validHeight) &&
                            tile->bytes[static_cast<std::size_t>(y) * core::kTileSize + static_cast<std::size_t>(x)] != 0) {
                            fail(name + ": ненулевая маска за краем документа");
                        }
                    }
                }
                mask.setTile(tx, ty, std::move(tile));
            }
            snapshot.selection = std::move(mask);
        }

        // Listed tiles and tile entries must match one to one.
        if (listed.size() != tileEntries.size()) fail("список тайлов в manifest.json не совпадает с содержимым архива");
        snapshot.tiles.reserve(listed.size());
        for (const TileRef& t : listed) {
            const std::string name = tilePath(t.layer, t.tx, t.ty);
            const std::optional<std::uint64_t> entry = src.size(name);
            if (!entry) fail("нет записи тайла " + name);
            if (*entry != core::kTileBytes) fail(std::format("{}: размер не {} байт", name, core::kTileBytes));
            const std::vector<std::byte> bytes = src.read(name);
            auto data = std::make_shared<core::TileData>();
            std::memcpy(data->halfs.data(), bytes.data(), core::kTileBytes);
            const std::string problem =
                core::validateTile(*data, core::tileExtent(snapshot.width, snapshot.height, t.tx, t.ty));
            if (!problem.empty()) fail(name + ": " + problem);
            if (snapshot.migratedFromLinear) {
                for (std::size_t i = 0; i < data->halfs.size(); i += 4) {
                    const auto p = core::linearPmaToInk(core::halfToFloat(data->halfs[i]), core::halfToFloat(data->halfs[i + 1]),
                                                        core::halfToFloat(data->halfs[i + 2]), core::halfToFloat(data->halfs[i + 3]));
                    for (std::size_t c = 0; c < 4; ++c) data->halfs[i + c] = core::floatToHalf(p[c]);
                }
            }
            snapshot.tiles.push_back({t.layer, t.tx, t.ty, std::move(data)});
        }
        if (snapshot.migratedFromLinear) {
            // Vector colours were straight linear light.
            for (auto& [layer, objects] : snapshot.vectors) {
                for (core::VectorObject& o : objects) {
                    for (int c = 0; c < 3; ++c) o.rgba[static_cast<std::size_t>(c)] = core::linearToSrgb(std::clamp(o.rgba[static_cast<std::size_t>(c)], 0.0, 1.0));
                }
            }
        }
        return snapshot;
    }
}

LoadResult loadProject(const fs::path& path) {
    LoadResult result;
    try {
        const ZipReader zip(path);
        EntryAccess src;
        src.list = [&] {
            std::vector<std::pair<std::string, std::uint64_t>> out;
            for (const ZipEntry& e : zip.entries()) out.emplace_back(e.name, e.uncompressedSize);
            return out;
        };
        src.size = [&](const std::string& name) -> std::optional<std::uint64_t> {
            const ZipEntry* e = zip.find(name);
            if (!e) return std::nullopt;
            return e->uncompressedSize;
        };
        src.read = [&](const std::string& name) {
            const ZipEntry* e = zip.find(name);
            if (!e) fail("нет записи " + name);
            return zip.read(*e);
        };
        result.snapshot = loadFrom(src);
        result.identity = identifyFile(path);
    } catch (const std::exception& error) {
        result.snapshot.reset();
        result.error = error.what();
    }
    return result;
}

LoadResult loadFromEntries(const Entries& entries) {
    LoadResult result;
    try {
        if (entries.size() > 1'000'000) fail("слишком много записей");
        EntryAccess src;
        src.list = [&] {
            std::vector<std::pair<std::string, std::uint64_t>> out;
            for (const auto& [name, bytes] : entries) out.emplace_back(name, bytes.size());
            return out;
        };
        src.size = [&](const std::string& name) -> std::optional<std::uint64_t> {
            const auto it = entries.find(name);
            if (it == entries.end()) return std::nullopt;
            return it->second.size();
        };
        src.read = [&](const std::string& name) {
            const auto it = entries.find(name);
            if (it == entries.end()) fail("нет записи " + name);
            return it->second;
        };
        result.snapshot = loadFrom(src);
    } catch (const std::exception& error) {
        result.snapshot.reset();
        result.error = error.what();
    }
    return result;
}

void forEachEntry(const ProjectSnapshot& snapshot, const EntryVisitor& visit) {
    const std::string manifest = manifestJson(snapshot);
    visit("manifest.json", std::as_bytes(std::span(manifest)));
    std::vector<const TileEntry*> ordered;
    for (const TileEntry& t : snapshot.tiles) {
        if (!t.data) fail("внутренняя ошибка: тайл без данных");
        ordered.push_back(&t);
    }
    std::sort(ordered.begin(), ordered.end(), [](const TileEntry* a, const TileEntry* b) {
        return std::tie(a->layer, a->ty, a->tx) < std::tie(b->layer, b->ty, b->tx);
    });
    for (const TileEntry* t : ordered) visit(tilePath(t->layer, t->tx, t->ty), std::as_bytes(std::span(t->data->halfs)));
    for (const LayerInfo& l : snapshot.layers) {
        if (l.type != LayerType::Vector) continue;
        Value list = Value::array();
        if (const auto it = snapshot.vectors.find(l.id); it != snapshot.vectors.end()) {
            for (const core::VectorObject& o : it->second) list.push(objectToJson(o));
        }
        const std::string text = json::serialize(list);
        visit(vectorPath(l.id), std::as_bytes(std::span(text)));
    }
    if (snapshot.selection) {
        for (const auto& [key, tile] : snapshot.selection->tiles()) {
            visit(std::format("selection/{}_{}.bin", key.first, key.second), std::as_bytes(std::span(tile->bytes)));
        }
    }
    if (snapshot.thumbnailPng && !snapshot.thumbnailPng->empty()) visit("thumbnail.png", *snapshot.thumbnailPng);
}

SaveResult saveProject(const fs::path& path, const ProjectSnapshot& snapshot, const SaveFault& fault) {
    SaveResult result;
    const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::current_path();
    const fs::path temp = directory / (path.filename().wstring() + L".tmp-" + fs::path(randomSuffix()).wstring());
    const fs::path backup = directory / (path.filename().wstring() + L".bak");

    // The temporary file lives in the destination directory, so publishing is a rename on one volume.
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = std::format("не удалось создать временный файл в «{}» (ошибка Windows {})", utf8(directory),
                                   GetLastError());
        return result;
    }
    bool keepTemp = false;
    try {
        ZipWriter writer(file);
        forEachEntry(snapshot, [&](const std::string& name, std::span<const std::byte> data) { writer.add(name, data); });
        writer.finish();
        if (fault && fault(SaveStage::Written)) fail("запись прервана (тест)");

        // Check what reached the file: the central directory and every CRC.
        {
            const ZipReader check(temp);
            if (check.entries().size() != writer.entryCount()) fail("проверка записанного файла: не те записи");
            for (const ZipEntry& e : check.entries()) (void)check.read(e);
        }
        if (fault && fault(SaveStage::Verified)) fail("запись прервана после проверки (тест)");
        if (!FlushFileBuffers(file)) fail(std::format("не удалось сбросить данные на диск (ошибка Windows {})", GetLastError()));
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (fault && fault(SaveStage::BeforeReplace)) fail("запись прервана перед заменой файла (тест)");

        std::error_code ec;
        if (fs::exists(path, ec)) {
            if (!ReplaceFileW(path.c_str(), temp.c_str(), backup.c_str(),
                              REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
                const DWORD code = GetLastError();
                // ReplaceFileW may already have moved the original to the backup name: put it back.
                if (!fs::exists(path, ec) && fs::exists(backup, ec)) {
                    MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
                }
                keepTemp = fs::exists(temp, ec);
                fail(std::format("не удалось заменить файл (ошибка Windows {}). Прежняя версия: {}. Новая версия: {}", code,
                                 fs::exists(path, ec) ? "на месте" : "не найдена",
                                 keepTemp ? "сохранена как «" + utf8(temp) + "»" : "не сохранена"));
            }
            DeleteFileW(backup.c_str());
        } else if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            keepTemp = fs::exists(temp, ec);
            fail(std::format("не удалось переименовать временный файл (ошибка Windows {}). Новая версия: {}", code,
                             keepTemp ? "сохранена как «" + utf8(temp) + "»" : "не сохранена"));
        }
        result.ok = true;
        result.identity = identifyFile(path);
    } catch (const std::exception& error) {
        result.ok = false;
        result.error = error.what();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!keepTemp) DeleteFileW(temp.c_str());
    }
    return result;
}

}  // namespace chroma::project

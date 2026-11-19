#include "project_io/BrushImport.hpp"

#include "core/Geometry.hpp"
#include "project_io/BrushJson.hpp"
#include "project_io/Inflate.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

namespace chroma::project {
namespace {

namespace fs = std::filesystem;
using json::Value;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

std::vector<std::byte> toBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string toText(std::span<const std::byte> b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

/// Percent-decoding once; '+' stays '+' (a Base64 value in a query keeps it, §14.1).
std::string percentDecode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

/// Strict Base64 (standard or URL alphabet); nullopt for anything else.
std::optional<std::string> base64Decode(std::string_view s, bool url) {
    std::string clean;
    for (const char c : s) {
        if (c == '\r' || c == '\n') continue;
        clean.push_back(c);
    }
    while (!clean.empty() && clean.back() == '=') clean.pop_back();
    if (clean.empty() || clean.size() % 4 == 1) return std::nullopt;
    const auto value = [url](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == (url ? '-' : '+')) return 62;
        if (c == (url ? '_' : '/')) return 63;
        return -1;
    };
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : clean) {
        const int v = value(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::size_t codePoints(const std::string& s) {
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

double numberIn(const Value& o, std::string_view key, double lo, double hi) {
    const Value* v = o.find(key);
    if (!v || !v->isNumber() || !std::isfinite(v->asNumber())) fail(std::format("PresetFormat: {} должен быть числом", key));
    const double x = v->asNumber();
    if (x < lo || x > hi) fail(std::format("PresetFormat: {} = {} вне [{}, {}]", key, x, lo, hi));
    return x;
}

void allowOnly(const Value& o, std::string_view field, std::initializer_list<std::string_view> keys) {
    if (!o.isObject()) fail(std::format("PresetFormat: {} должен быть объектом", field));
    for (const std::string& k : o.keys()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) fail(std::format("PresetFormat: неизвестное поле {}.{}", field, k));
    }
}

std::vector<core::PressurePoint> curveIn(const Value& v, std::string_view key) {
    if (!v.isArray() || v.items().size() < 2 || v.items().size() > 32) fail(std::format("PresetFormat: {} — от 2 до 32 пар", key));
    std::vector<core::PressurePoint> c;
    for (const Value& p : v.items()) {
        if (!p.isArray() || p.items().size() != 2 || !p.items()[0].isNumber() || !p.items()[1].isNumber()) {
            fail(std::format("PresetFormat: {} — пары чисел", key));
        }
        c.push_back({p.items()[0].asNumber(), p.items()[1].asNumber()});
    }
    try {
        core::validateCurve(c);  // x strictly increasing from 0 to 1, values in [0, 1]
    } catch (const std::exception&) {
        fail(std::format("PresetFormat: {}: X должен строго возрастать от 0 до 1, значения в [0, 1]", key));
    }
    return c;
}

int blendFrom(const Value& v) {
    static constexpr std::array<std::string_view, 27> kIds = {
        "normal",     "multiply",      "screen",       "overlay",     "add",        "color-dodge", "linear-burn",
        "darken",     "lighten",       "color-burn",   "darker-color", "lighter-color", "soft-light", "hard-light",
        "vivid-light", "linear-light", "pin-light",    "hard-mix",    "difference", "exclusion",   "subtract",
        "divide",     "hue",           "saturation",   "color",       "luminosity", "invert"};
    if (const auto id = v.asInteger(0, 26)) return static_cast<int>(*id);
    if (v.isString()) {
        std::string s = v.asString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(c == '_' || c == ' ' ? '-' : std::tolower(c)); });
        if (s == "linear-dodge") s = "add";
        const auto it = std::find(kIds.begin(), kIds.end(), s);
        if (it != kIds.end()) return static_cast<int>(it - kIds.begin());
    }
    fail("PresetFormat: blend — целое 0–26 или имя режима §7");
}

std::uint32_t be32(std::span<const std::byte> b, std::size_t at) {
    return (std::to_integer<std::uint32_t>(b[at]) << 24) | (std::to_integer<std::uint32_t>(b[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(b[at + 2]) << 8) | std::to_integer<std::uint32_t>(b[at + 3]);
}

ImportResult importJsonText(std::string_view text, AssetStore& assets) {
    ImportResult r;
    r.source = toBytes(text);
    try {
        const Value v = json::parse(text);
        if (v.isObject() && v.find("format")) {
            r.profile = mapPresetFormat(v, assets);
        } else if (v.isObject() && v.find("brushes")) {
            const auto brushes = catalogFromJson(v);
            if (brushes.size() != 1) fail("в файле несколько пресетов: импортируйте их по одному");
            r.profile = brushes.front();
        } else {
            r.profile = brushFromJson(v);
        }
        // Masks must be local before anything is published (§14.1).
        for (const auto* mask : {r.profile->tip.maskId ? &*r.profile->tip.maskId : nullptr,
                                 r.profile->grain && r.profile->grain->maskId ? &*r.profile->grain->maskId : nullptr}) {
            if (mask && !assets.contains(*mask)) fail("маска " + mask->substr(0, 12) + "… не найдена в локальных ресурсах");
        }
        r.fidelity = "mapped";
    } catch (const std::exception& e) {
        r.profile.reset();
        r.error = e.what();
    }
    return r;
}

ImportResult importIpb(std::span<const std::byte> bytes) {
    ImportResult r;
    r.source.assign(bytes.begin(), bytes.end());
    try {
        if (bytes.size() < 8) fail("IPBZ: запись слишком коротка");
        const std::uint32_t version = be32(bytes, 4);
        if (version != 80100 && version != 120200) fail(std::format("IPBZ: неизвестная версия {}", version));
        const auto payload = bytes.subspan(8);
        std::vector<std::byte> body;
        const bool raw = payload.size() >= 4 && std::to_integer<int>(payload[0]) == 1 && std::to_integer<int>(payload[1]) == 0 &&
                         std::to_integer<int>(payload[2]) == 2 && std::to_integer<int>(payload[3]) == 2;
        if (raw) {
            body.assign(payload.begin(), payload.end());
        } else {
            try {
                body = inflateRawUpTo(payload, 1 << 20);
            } catch (const std::exception&) {
                fail("IPBZ: повреждённый или оборванный поток DEFLATE");
            }
        }
        // §14.1, the observed layout: 01 00 02 02, BE32 length−12 at 4, BE float32 diameter at 20,
        // BE16 name length at 196, UTF-8 name from 198, last BE32 = (4 − length) mod 2³².
        const std::size_t len = body.size();
        if (len < 202 || std::to_integer<int>(body[0]) != 1 || std::to_integer<int>(body[1]) != 0 ||
            std::to_integer<int>(body[2]) != 2 || std::to_integer<int>(body[3]) != 2) {
            fail("IPBZ: неизвестный вариант тела записи");
        }
        if (be32(body, 4) != len - 12) fail("IPBZ: длина тела не совпадает с заголовком");
        if (be32(body, len - 4) != static_cast<std::uint32_t>(4 - static_cast<std::int64_t>(len))) fail("IPBZ: неверный конец записи");
        const float diameter = std::bit_cast<float>(be32(body, 20));
        const std::size_t nameLength = (std::to_integer<std::size_t>(body[196]) << 8) | std::to_integer<std::size_t>(body[197]);
        if (198 + nameLength > len - 4) fail("IPBZ: длина имени выходит за запись");
        std::string name(reinterpret_cast<const char*>(body.data() + 198), nameLength);
        if (!std::isfinite(diameter) || diameter < 0.25f || diameter > 2048.0f) fail("IPBZ: диаметр вне 0.25–2048 px");
        if (name.empty() || codePoints(name) > 80) fail("IPBZ: имя должно быть от 1 до 80 символов");
        core::BrushProfile p = core::builtInBrushes().front();  // the basic pen: name and size recovered
        p.name = name;
        p.tip.diameter = diameter;
        p.id = userBrushId(p);
        core::validate(p);
        r.profile = p;
        r.fidelity = "basic";
    } catch (const std::exception& e) {
        r.profile.reset();
        r.error = e.what();
    }
    return r;
}

}  // namespace

double emaTau(double alpha, double sampleMs) { return alpha >= 1.0 ? 0.0 : -sampleMs / std::log1p(-alpha); }

std::string userBrushId(const core::BrushProfile& profile) {
    core::BrushProfile normalised = profile;
    normalised.id = "user.x";
    const std::string text = json::serialize(brushToJson(normalised));
    return "user." + sha256Hex(std::as_bytes(std::span(text))).substr(0, 24);
}

core::BrushProfile mapPresetFormat(const Value& v, AssetStore& assets) {
    allowOnly(v, "preset",
              {"format", "version", "family", "name", "sizePx", "aspect", "hardnessPct", "angleDeg", "orientation", "opacityPct",
               "flowPct", "minSizePct", "minOpacityPct", "spacingPct", "smoothing", "fade", "sizeCurve", "opacityCurve", "blend",
               "accumulation", "textureId", "textureRole", "maskChannel", "grain", "wet", "tone"});
    const Value* format = v.find("format");
    if (!format || !format->isString() || format->asString() != "PresetFormat") fail("неизвестный формат пресета");
    const Value* version = v.find("version");
    if (!version || !(version->asInteger(1, 1))) fail("PresetFormat: поддерживается только version 1");
    const Value* family = v.find("family");
    const Value* name = v.find("name");
    if (!family || !family->isString() || !name || !name->isString()) fail("PresetFormat: обязательны family и name");
    const auto& catalog = core::builtInBrushes();
    const auto base = std::find_if(catalog.begin(), catalog.end(), [&](const core::BrushProfile& b) { return b.id == "core." + family->asString(); });
    if (base == catalog.end()) fail("PresetFormat: неизвестное семейство " + family->asString());
    core::BrushProfile p = *base;  // unset fields inherit the family's master profile
    p.name = name->asString();
    if (p.name.empty() || codePoints(p.name) > 80) fail("PresetFormat: name должен быть от 1 до 80 символов");

    if (v.find("sizePx")) p.tip.diameter = numberIn(v, "sizePx", 0.25, 2048);
    if (v.find("aspect")) p.tip.aspect = numberIn(v, "aspect", 0.05, 1);
    if (v.find("hardnessPct")) p.tip.hardness = numberIn(v, "hardnessPct", 0, 100) / 100;
    if (v.find("angleDeg")) p.tip.angleDeg = numberIn(v, "angleDeg", -180, 180);
    if (const Value* o = v.find("orientation")) {
        const std::string s = o->isString() ? o->asString() : "";
        if (s == "fixed") p.tip.orientation = core::TipOrientation::Fixed;
        else if (s == "tilt") p.tip.orientation = core::TipOrientation::Tilt;
        else if (s == "path") p.tip.orientation = core::TipOrientation::Path;
        else fail("PresetFormat: orientation — fixed, tilt или path");
    }
    if (v.find("opacityPct")) p.ink.opacity = numberIn(v, "opacityPct", 0, 100) / 100;
    if (v.find("flowPct")) p.ink.flow = numberIn(v, "flowPct", 0, 100) / 100;
    if (v.find("minSizePct")) p.ink.minSize = numberIn(v, "minSizePct", 0, 100) / 100;
    if (v.find("minOpacityPct")) p.ink.minOpacity = numberIn(v, "minOpacityPct", 0, 100) / 100;
    if (v.find("spacingPct")) p.ink.spacing = numberIn(v, "spacingPct", 1, 200) / 100;
    if (const Value* s = v.find("smoothing")) {
        allowOnly(*s, "smoothing", {"method", "timeMs", "alpha", "sampleMs"});
        const Value* method = s->find("method");
        const std::string m = method && method->isString() ? method->asString() : "";
        if (m == "off") {
            p.stabilizer = {core::StabilizerMethod::Off, 0.0};
        } else if (m == "window" || m == "ema") {
            p.stabilizer.method = m == "ema" ? core::StabilizerMethod::Ema : core::StabilizerMethod::Window;
            if (m == "ema" && s->find("alpha")) {
                const double alpha = numberIn(*s, "alpha", 0, 1);
                if (!(alpha > 0)) fail("PresetFormat: smoothing.alpha должен быть в (0, 1]");
                const double tau = emaTau(alpha, numberIn(*s, "sampleMs", 0.01, 250));
                if (tau > 200) fail("PresetFormat: сглаживание дольше 200 мс");
                p.stabilizer.timeMs = tau;
            } else {
                p.stabilizer.timeMs = numberIn(*s, "timeMs", 0, 200);
            }
        } else {
            fail("PresetFormat: smoothing.method — off, window или ema");
        }
    }
    if (const Value* f = v.find("fade")) {
        allowOnly(*f, "fade", {"startPx", "endPx", "liftGain", "shape", "radiusPower", "opacityPower"});
        if (f->find("startPx")) p.taper.startPx = numberIn(*f, "startPx", 0, 2048);
        if (f->find("endPx")) p.taper.endPx = numberIn(*f, "endPx", 0, 2048);
        if (f->find("liftGain")) p.taper.liftGain = numberIn(*f, "liftGain", 0, 3);
        if (f->find("radiusPower")) p.taper.radiusPower = numberIn(*f, "radiusPower", 0, 4);
        if (f->find("opacityPower")) p.taper.opacityPower = numberIn(*f, "opacityPower", 0, 4);
        if (const Value* shape = f->find("shape")) {
            const std::string s = shape->isString() ? shape->asString() : "";
            if (s != "linear" && s != "smooth") fail("PresetFormat: fade.shape — linear или smooth");
            p.taper.shape = s == "linear" ? core::TaperShape::Linear : core::TaperShape::Smooth;
        }
    }
    if (const Value* c = v.find("sizeCurve")) p.sizeCurve = curveIn(*c, "sizeCurve");
    if (const Value* c = v.find("opacityCurve")) p.opacityCurve = curveIn(*c, "opacityCurve");
    if (const Value* b = v.find("blend")) p.ink.blendId = blendFrom(*b);
    if (const Value* a = v.find("accumulation")) {
        const std::string s = a->isString() ? a->asString() : "";
        if (s != "max" && s != "build") fail("PresetFormat: accumulation — max или build");
        p.ink.accumulate = s == "max" ? core::Accumulate::Max : core::Accumulate::Build;
    }
    if (const Value* w = v.find("wet")) {
        if (!p.wet) fail("PresetFormat: wet только для семейства watercolor");
        allowOnly(*w, "wet", {"blending", "dilution", "bleed", "strength"});
        if (w->find("blending")) p.wet->blending = numberIn(*w, "blending", 0, 1);
        if (w->find("dilution")) p.wet->dilution = numberIn(*w, "dilution", 0, 1);
        if (w->find("bleed")) p.wet->bleed = numberIn(*w, "bleed", 0, 1);
        if (w->find("strength")) p.wet->strength = numberIn(*w, "strength", 0, 1);
    }
    bool grainMaskSet = false;
    if (const Value* g = v.find("grain")) {
        if (!p.grain) fail("PresetFormat: grain только для семейства texture");
        allowOnly(*g, "grain", {"grainPx", "grainAmount", "seed", "maskId"});
        if (g->find("grainPx")) p.grain->grainPx = numberIn(*g, "grainPx", 0.25, 512);
        if (g->find("grainAmount")) p.grain->grainAmount = numberIn(*g, "grainAmount", 0, 1);
        if (const Value* seed = g->find("seed")) {
            const auto s = seed->asInteger(0, 4294967295LL);
            if (!s) fail("PresetFormat: grain.seed — uint32");
            p.grain->seed = static_cast<std::uint32_t>(*s);
        }
        if (const Value* mask = g->find("maskId"); mask && !mask->isNull()) {
            if (!mask->isString() || !core::isSha256(mask->asString())) fail("PresetFormat: grain.maskId — null или SHA-256");
            p.grain->maskId = mask->asString();
            grainMaskSet = true;
        }
    }
    if (const Value* t = v.find("tone")) {
        if (!p.tone) fail("PresetFormat: tone только для семейства screentone");
        allowOnly(*t, "tone", {"periodPx", "density", "toneAngle"});
        if (t->find("periodPx")) p.tone->periodPx = numberIn(*t, "periodPx", 2, 256);
        if (t->find("density")) p.tone->density = numberIn(*t, "density", 0, 1);
        if (t->find("toneAngle")) p.tone->toneAngleDeg = numberIn(*t, "toneAngle", -180, 180);
    }
    if (const Value* texture = v.find("textureId")) {
        // A SHA-256, a local file stem, or a non-negative number of a local texture (§13 aliases).
        std::optional<std::string> sha;
        if (texture->isString() && core::isSha256(texture->asString())) {
            sha = texture->asString();
        } else if (texture->isString()) {
            sha = assets.byName(texture->asString());
        } else if (const auto n = texture->asInteger(0, std::numeric_limits<std::int64_t>::max())) {
            sha = assets.byName(std::format("texture_{}", *n));
        }
        if (!sha || !assets.contains(*sha)) fail("PresetFormat: текстура не найдена в локальных ресурсах");
        const Value* role = v.find("textureRole");
        const std::string r = role && role->isString() ? role->asString() : "tip";
        if (r == "grain") {
            if (!p.grain) fail("PresetFormat: textureRole grain требует семейство texture");
            if (grainMaskSet) fail("PresetFormat: маска зерна задана дважды (grain.maskId и textureRole=grain)");
            p.grain->maskId = *sha;  // the channel is fixed: redAlpha
        } else if (r == "tip") {
            p.tip.maskId = *sha;
            p.tip.maskChannel = core::MaskChannel::Alpha;
            if (const Value* ch = v.find("maskChannel")) {
                const std::string c = ch->isString() ? ch->asString() : "";
                if (c == "alpha") p.tip.maskChannel = core::MaskChannel::Alpha;
                else if (c == "redAlpha") p.tip.maskChannel = core::MaskChannel::RedAlpha;
                else if (c == "inverseLumaAlpha") p.tip.maskChannel = core::MaskChannel::InverseLumaAlpha;
                else fail("PresetFormat: maskChannel — alpha, redAlpha или inverseLumaAlpha");
            }
        } else {
            fail("PresetFormat: textureRole — tip или grain");
        }
    } else if (v.find("textureRole") || v.find("maskChannel")) {
        fail("PresetFormat: textureRole и maskChannel требуют textureId");
    }
    p.id = userBrushId(p);
    try {
        core::validate(p);
    } catch (const std::exception& e) {
        fail(std::string("PresetFormat: результат вне контракта §6: ") + e.what());
    }
    return p;
}

ImportResult importFromPayload(std::string_view input, AssetStore& assets) {
    const std::string text = trim(input);
    const auto fromDecoded = [&](const std::optional<std::string>& decoded) {
        if (!decoded) {
            ImportResult r;
            r.error = "строгий Base64 не декодируется";
            return r;
        }
        return importJsonText(*decoded, assets);
    };
    if (text.starts_with("data:application/json;base64,")) return fromDecoded(base64Decode(text.substr(29), false));
    if (text.starts_with("data:application/json,")) return importJsonText(percentDecode(text.substr(22)), assets);
    if (text.starts_with("base64url:")) return fromDecoded(base64Decode(text.substr(10), true));
    if (text.starts_with("base64:")) return fromDecoded(base64Decode(text.substr(7), false));
    if (text.find("://") != std::string::npos) {
        // A link: exactly one preset= or data= after ? or #; a page link alone has no local data.
        std::optional<std::string> value;
        int found = 0;
        const std::size_t start = text.find_first_of("?#");
        if (start != std::string::npos) {
            std::string_view params = std::string_view(text).substr(start + 1);
            while (!params.empty()) {
                const std::size_t end = params.find_first_of("&#");
                const std::string_view pair = params.substr(0, end);
                for (const std::string_view key : {std::string_view("preset="), std::string_view("data=")}) {
                    if (pair.starts_with(key)) {
                        value = percentDecode(pair.substr(key.size()));
                        ++found;
                    }
                }
                if (end == std::string_view::npos) break;
                params.remove_prefix(end + 1);
            }
        }
        if (found != 1) {
            ImportResult r;
            r.error = found == 0 ? "в ссылке нет локальных данных пресета (параметр preset или data)" : "в ссылке несколько параметров пресета";
            return r;
        }
        return importFromPayload(*value, assets);
    }
    if (text.starts_with("{")) return importJsonText(text, assets);
    if (text.starts_with("%7B") || text.starts_with("%7b")) return importJsonText(percentDecode(text), assets);
    if (auto plain = base64Decode(text, false)) return importJsonText(*plain, assets);
    if (auto url = base64Decode(text, true)) return importJsonText(*url, assets);
    ImportResult r;
    r.error = "данные не похожи ни на пресет, ни на ссылку с пресетом";
    return r;
}

ImportResult importFromBytes(std::span<const std::byte> bytes, AssetStore& assets) {
    static constexpr std::array<std::uint8_t, 4> kIpbz = {0x49, 0x50, 0x42, 0x5A};
    static constexpr std::array<std::uint8_t, 8> kPng = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const auto startsWith = [&](const auto& sig) {
        if (bytes.size() < sig.size()) return false;
        for (std::size_t i = 0; i < sig.size(); ++i) {
            if (std::to_integer<std::uint8_t>(bytes[i]) != sig[i]) return false;
        }
        return true;
    };
    if (startsWith(kIpbz)) return importIpb(bytes);
    if (startsWith(kPng)) {
        ImportResult r;
        r.source.assign(bytes.begin(), bytes.end());
        r.error = "PNG-карточки с QR-кодом пока не поддерживаются: в этой сборке нет локального декодера QR";
        return r;
    }
    ImportResult r = importFromPayload(toText(bytes), assets);
    r.source.assign(bytes.begin(), bytes.end());
    return r;
}

ImportResult importFromFile(const fs::path& path, AssetStore& assets) {
    const auto bytes = readFileBytes(path);
    if (!bytes) {
        ImportResult r;
        r.error = "файл не читается";
        return r;
    }
    if (bytes->size() > (16u << 20)) {
        ImportResult r;
        r.error = "файл пресета больше 16 МиБ";
        return r;
    }
    return importFromBytes(*bytes, assets);
}

std::vector<LibraryEntry> loadLibrary(const fs::path& directory, std::string& error) {
    error.clear();
    std::vector<LibraryEntry> entries;
    const auto bytes = readFileBytes(directory / L"user_brushes.json");
    if (!bytes) return entries;  // no library yet
    try {
        const Value v = json::parse(toText(*bytes));
        const Value* list = v.isObject() ? v.find("entries") : nullptr;
        if (!list || !list->isArray()) fail("user_brushes.json: нет массива entries");
        for (const Value& e : list->items()) {
            const Value* profile = e.find("profile");
            const Value* sha = e.find("sourceSha256");
            const Value* fidelity = e.find("fidelity");
            if (!profile || !sha || !sha->isString() || !fidelity || !fidelity->isString()) fail("user_brushes.json: неполная запись");
            entries.push_back({brushFromJson(*profile), sha->asString(), fidelity->asString()});
        }
    } catch (const std::exception& e) {
        error = e.what();
        entries.clear();
    }
    return entries;
}

std::string saveUserBrush(const fs::path& directory, const ImportResult& result) {
    if (!result.profile) return "нечего сохранять";
    std::error_code ec;
    fs::create_directories(directory / L"sources", ec);
    std::string error;
    std::vector<LibraryEntry> entries = loadLibrary(directory, error);
    if (!error.empty()) return "библиотека повреждена, запись отменена: " + error;  // the old bytes stay
    const std::string sha = sha256Hex(result.source);
    // The source payload, by its hash.
    const fs::path source = directory / L"sources" / (sha + ".bin");
    if (!fs::exists(source, ec)) {
        std::ofstream out(source, std::ios::binary);
        out.write(reinterpret_cast<const char*>(result.source.data()), static_cast<std::streamsize>(result.source.size()));
        if (!out) return "не удалось записать исходные данные пресета";
    }
    std::erase_if(entries, [&](const LibraryEntry& e) { return e.profile.id == result.profile->id; });
    entries.push_back({*result.profile, sha, result.fidelity});
    Value root = Value::object();
    root.set("version", Value::number(1));
    Value& list = root.set("entries", Value::array());
    for (const LibraryEntry& e : entries) {
        Value item = Value::object();
        item.set("profile", brushToJson(e.profile));
        item.set("sourceSha256", Value::string(e.sourceSha256));
        item.set("fidelity", Value::string(e.fidelity));
        list.push(std::move(item));
    }
    const std::string text = json::serialize(root);
    // Atomic publication: a temporary file in the same directory, then a replacing rename.
    const fs::path target = directory / L"user_brushes.json";
    const fs::path temp = directory / L"user_brushes.json.tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) return "не удалось записать библиотеку кистей";
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp, ec);
        return std::format("не удалось заменить user_brushes.json (ошибка Windows {})", GetLastError());
    }
    return {};
}

}  // namespace chroma::project

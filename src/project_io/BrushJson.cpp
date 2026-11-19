#include "project_io/BrushJson.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

namespace chroma::project {
namespace {

using json::Value;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("кисть: " + what); }

template <class E>
struct EnumName {
    E value;
    std::string_view name;
};

constexpr EnumName<core::BrushFamily> kFamilies[] = {
    {core::BrushFamily::Dip, "dip"},           {core::BrushFamily::Fade, "fade"},
    {core::BrushFamily::Fountain, "fountain"}, {core::BrushFamily::Airbrush, "airbrush"},
    {core::BrushFamily::Watercolor, "watercolor"}, {core::BrushFamily::Texture, "texture"},
    {core::BrushFamily::Screentone, "screentone"}};
constexpr EnumName<core::TipOrientation> kOrientations[] = {
    {core::TipOrientation::Fixed, "fixed"}, {core::TipOrientation::Tilt, "tilt"}, {core::TipOrientation::Path, "path"}};
constexpr EnumName<core::Accumulate> kAccumulate[] = {{core::Accumulate::Max, "max"}, {core::Accumulate::Build, "build"}};
constexpr EnumName<core::StabilizerMethod> kMethods[] = {
    {core::StabilizerMethod::Off, "off"}, {core::StabilizerMethod::Ema, "ema"}, {core::StabilizerMethod::Window, "window"}};
constexpr EnumName<core::TaperShape> kShapes[] = {{core::TaperShape::Linear, "linear"}, {core::TaperShape::Smooth, "smooth"}};
constexpr EnumName<core::MaskChannel> kChannels[] = {{core::MaskChannel::Alpha, "alpha"},
                                                     {core::MaskChannel::RedAlpha, "redAlpha"},
                                                     {core::MaskChannel::InverseLumaAlpha, "inverseLumaAlpha"}};

template <class E, std::size_t N>
std::string_view nameOf(const EnumName<E> (&table)[N], E value) {
    for (const auto& e : table) {
        if (e.value == value) return e.name;
    }
    return table[0].name;
}

template <class E, std::size_t N>
E valueOf(const EnumName<E> (&table)[N], const Value& v, std::string_view field) {
    if (v.isString()) {
        for (const auto& e : table) {
            if (e.name == v.asString()) return e.value;
        }
    }
    fail(std::format("недопустимое значение {}", field));
}

/// An object with exactly the allowed keys, of which `required` must be present.
const Value& object(const Value& v, std::string_view field, std::initializer_list<std::string_view> allowed,
                    std::initializer_list<std::string_view> required) {
    if (!v.isObject()) fail(std::format("{} должен быть объектом", field));
    for (const std::string& key : v.keys()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) fail(std::format("лишнее поле {}.{}", field, key));
    }
    for (const std::string_view key : required) {
        if (!v.find(key)) fail(std::format("нет поля {}.{}", field, key));
    }
    return v;
}

double number(const Value& o, std::string_view key) {
    const Value* v = o.find(key);
    if (!v || !v->isNumber()) fail(std::format("{} должен быть числом", key));
    return v->asNumber();
}

std::vector<core::PressurePoint> curveFrom(const Value& v, std::string_view field) {
    if (!v.isArray() || v.items().size() < 2 || v.items().size() > 32) fail(std::format("{}: от 2 до 32 точек", field));
    std::vector<core::PressurePoint> out;
    for (const Value& p : v.items()) {
        if (!p.isArray() || p.items().size() != 2 || !p.items()[0].isNumber() || !p.items()[1].isNumber()) {
            fail(std::format("{}: точка — пара чисел", field));
        }
        out.push_back({p.items()[0].asNumber(), p.items()[1].asNumber()});
    }
    return out;
}

Value curveTo(const std::vector<core::PressurePoint>& curve) {
    Value a = Value::array();
    for (const auto& p : curve) {
        Value pair = Value::array();
        pair.push(Value::number(p[0]));
        pair.push(Value::number(p[1]));
        a.push(std::move(pair));
    }
    return a;
}

}  // namespace

Value brushToJson(const core::BrushProfile& p) {
    Value b = Value::object();
    b.set("id", Value::string(p.id));
    b.set("name", Value::string(p.name));
    b.set("family", Value::string(std::string(nameOf(kFamilies, p.family))));
    if (!p.sizeCurve.empty() || !p.opacityCurve.empty()) {
        Value pressure = Value::object();
        if (!p.sizeCurve.empty()) pressure.set("size", curveTo(p.sizeCurve));
        if (!p.opacityCurve.empty()) pressure.set("opacity", curveTo(p.opacityCurve));
        b.set("pressure", std::move(pressure));
    }
    Value tip = Value::object();
    tip.set("diameter", Value::number(p.tip.diameter));
    tip.set("aspect", Value::number(p.tip.aspect));
    tip.set("hardness", Value::number(p.tip.hardness));
    tip.set("angleDeg", Value::number(p.tip.angleDeg));
    tip.set("orientation", Value::string(std::string(nameOf(kOrientations, p.tip.orientation))));
    if (p.tip.maskId) {
        tip.set("maskId", Value::string(*p.tip.maskId));
        tip.set("maskChannel", Value::string(std::string(nameOf(kChannels, p.tip.maskChannel))));
    }
    b.set("tip", std::move(tip));
    Value ink = Value::object();
    ink.set("opacity", Value::number(p.ink.opacity));
    ink.set("flow", Value::number(p.ink.flow));
    ink.set("accumulate", Value::string(std::string(nameOf(kAccumulate, p.ink.accumulate))));
    ink.set("minSize", Value::number(p.ink.minSize));
    ink.set("minOpacity", Value::number(p.ink.minOpacity));
    ink.set("spacing", Value::number(p.ink.spacing));
    if (p.ink.blendId != 0) ink.set("blendId", Value::number(p.ink.blendId));
    b.set("ink", std::move(ink));
    Value stabilizer = Value::object();
    stabilizer.set("method", Value::string(std::string(nameOf(kMethods, p.stabilizer.method))));
    stabilizer.set("timeMs", Value::number(p.stabilizer.timeMs));
    b.set("stabilizer", std::move(stabilizer));
    Value taper = Value::object();
    taper.set("startPx", Value::number(p.taper.startPx));
    taper.set("endPx", Value::number(p.taper.endPx));
    taper.set("liftGain", Value::number(p.taper.liftGain));
    taper.set("shape", Value::string(std::string(nameOf(kShapes, p.taper.shape))));
    taper.set("radiusPower", Value::number(p.taper.radiusPower));
    taper.set("opacityPower", Value::number(p.taper.opacityPower));
    b.set("taper", std::move(taper));
    if (p.wet) {
        Value wet = Value::object();
        wet.set("blending", Value::number(p.wet->blending));
        wet.set("dilution", Value::number(p.wet->dilution));
        wet.set("bleed", Value::number(p.wet->bleed));
        wet.set("strength", Value::number(p.wet->strength));
        b.set("wet", std::move(wet));
    }
    if (p.grain) {
        Value grain = Value::object();
        grain.set("grainPx", Value::number(p.grain->grainPx));
        grain.set("grainAmount", Value::number(p.grain->grainAmount));
        grain.set("seed", Value::number(p.grain->seed));
        grain.set("maskId", p.grain->maskId ? Value::string(*p.grain->maskId) : Value::null());
        b.set("grain", std::move(grain));
    }
    if (p.tone) {
        Value tone = Value::object();
        tone.set("periodPx", Value::number(p.tone->periodPx));
        tone.set("density", Value::number(p.tone->density));
        tone.set("toneAngle", Value::number(p.tone->toneAngleDeg));
        b.set("tone", std::move(tone));
    }
    return b;
}

core::BrushProfile brushFromJson(const Value& v) {
    const Value& b = object(v, "brush",
                            {"id", "name", "family", "pressure", "tip", "ink", "stabilizer", "taper", "wet", "grain", "tone"},
                            {"id", "name", "family", "tip", "ink", "stabilizer", "taper"});
    core::BrushProfile p;
    const Value& id = *b.find("id");
    const Value& name = *b.find("name");
    if (!id.isString() || !name.isString()) fail("id и name должны быть строками");
    p.id = id.asString();
    p.name = name.asString();
    p.family = valueOf(kFamilies, *b.find("family"), "family");
    if (const Value* pressure = b.find("pressure")) {
        object(*pressure, "pressure", {"size", "opacity"}, {});
        if (pressure->keys().empty()) fail("pressure не может быть пустым");
        if (const Value* size = pressure->find("size")) p.sizeCurve = curveFrom(*size, "pressure.size");
        if (const Value* opacity = pressure->find("opacity")) p.opacityCurve = curveFrom(*opacity, "pressure.opacity");
    }
    const Value& tip = object(*b.find("tip"), "tip", {"diameter", "aspect", "hardness", "angleDeg", "orientation", "maskId", "maskChannel"},
                              {"diameter", "aspect", "hardness", "angleDeg", "orientation"});
    // §6.1: maskId and maskChannel come together.
    if (static_cast<bool>(tip.find("maskId")) != static_cast<bool>(tip.find("maskChannel"))) fail("tip.maskId и tip.maskChannel задаются вместе");
    if (const Value* mask = tip.find("maskId")) {
        if (!mask->isString() || !core::isSha256(mask->asString())) fail("tip.maskId должен быть SHA-256");
        p.tip.maskId = mask->asString();
        p.tip.maskChannel = valueOf(kChannels, *tip.find("maskChannel"), "tip.maskChannel");
    }
    p.tip.diameter = number(tip, "diameter");
    p.tip.aspect = number(tip, "aspect");
    p.tip.hardness = number(tip, "hardness");
    p.tip.angleDeg = number(tip, "angleDeg");
    p.tip.orientation = valueOf(kOrientations, *tip.find("orientation"), "tip.orientation");
    const Value& ink = object(*b.find("ink"), "ink", {"opacity", "flow", "accumulate", "minSize", "minOpacity", "spacing", "blendId"},
                              {"opacity", "flow", "accumulate", "minSize", "minOpacity", "spacing"});
    if (const Value* blend = ink.find("blendId")) {
        const auto mode = blend->asInteger(0, 26);
        if (!mode) fail("ink.blendId должен быть целым от 0 до 26");
        p.ink.blendId = static_cast<int>(*mode);
    }
    p.ink.opacity = number(ink, "opacity");
    p.ink.flow = number(ink, "flow");
    p.ink.accumulate = valueOf(kAccumulate, *ink.find("accumulate"), "ink.accumulate");
    p.ink.minSize = number(ink, "minSize");
    p.ink.minOpacity = number(ink, "minOpacity");
    p.ink.spacing = number(ink, "spacing");
    const Value& stabilizer = object(*b.find("stabilizer"), "stabilizer", {"method", "timeMs"}, {"method", "timeMs"});
    p.stabilizer.method = valueOf(kMethods, *stabilizer.find("method"), "stabilizer.method");
    p.stabilizer.timeMs = number(stabilizer, "timeMs");
    const Value& taper = object(*b.find("taper"), "taper", {"startPx", "endPx", "liftGain", "shape", "radiusPower", "opacityPower"},
                                {"startPx", "endPx", "liftGain", "shape", "radiusPower", "opacityPower"});
    p.taper.startPx = number(taper, "startPx");
    p.taper.endPx = number(taper, "endPx");
    p.taper.liftGain = number(taper, "liftGain");
    p.taper.shape = valueOf(kShapes, *taper.find("shape"), "taper.shape");
    p.taper.radiusPower = number(taper, "radiusPower");
    p.taper.opacityPower = number(taper, "opacityPower");
    if (const Value* wet = b.find("wet")) {
        object(*wet, "wet", {"blending", "dilution", "bleed", "strength"}, {"blending", "dilution", "bleed", "strength"});
        p.wet = core::Wet{number(*wet, "blending"), number(*wet, "dilution"), number(*wet, "bleed"), number(*wet, "strength")};
    }
    if (const Value* grain = b.find("grain")) {
        object(*grain, "grain", {"grainPx", "grainAmount", "seed", "maskId"}, {"grainPx", "grainAmount", "seed", "maskId"});
        const auto seed = grain->find("seed")->asInteger(0, 4294967295LL);
        if (!seed) fail("grain.seed должен быть целым от 0 до 4294967295");
        p.grain = core::Grain{number(*grain, "grainPx"), number(*grain, "grainAmount"), static_cast<std::uint32_t>(*seed), {}};
        if (const Value* mask = grain->find("maskId"); !mask->isNull()) {
            if (!mask->isString() || !core::isSha256(mask->asString())) fail("grain.maskId должен быть null или SHA-256");
            p.grain->maskId = mask->asString();
        }
    }
    if (const Value* tone = b.find("tone")) {
        object(*tone, "tone", {"periodPx", "density", "toneAngle"}, {"periodPx", "density", "toneAngle"});
        p.tone = core::Tone{number(*tone, "periodPx"), number(*tone, "density"), number(*tone, "toneAngle")};
    }
    try {
        core::validate(p);  // §6.1 ranges and family rules
    } catch (const std::exception& error) {
        fail(std::format("«{}»: поле {} вне контракта §6.1", p.id, error.what()));
    }
    return p;
}

Value catalogToJson(const std::vector<core::BrushProfile>& brushes) {
    Value c = Value::object();
    c.set("version", Value::number(1));
    Value& list = c.set("brushes", Value::array());
    for (const auto& b : brushes) list.push(brushToJson(b));
    return c;
}

std::vector<core::BrushProfile> catalogFromJson(const Value& v) {
    const Value& c = object(v, "catalog", {"version", "brushes"}, {"version", "brushes"});
    const auto version = c.find("version")->asInteger(0, 1000000);
    if (!version || *version != 1) fail("неизвестная версия каталога кистей");
    const Value& list = *c.find("brushes");
    if (!list.isArray() || list.items().empty() || list.items().size() > 4096) fail("каталог должен содержать от 1 до 4096 кистей");
    std::vector<core::BrushProfile> out;
    std::set<std::string> ids;
    for (const Value& b : list.items()) {
        out.push_back(brushFromJson(b));
        if (!ids.insert(out.back().id).second) fail(std::format("повтор id {}", out.back().id));
    }
    return out;
}

}  // namespace chroma::project

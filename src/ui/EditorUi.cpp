#include "ui/EditorUi.hpp"

#include "core/BrushProfile.hpp"
#include "ui/Icons.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <format>
#include <functional>
#include <iterator>
#include <numbers>

namespace chroma::ui {
namespace {

namespace Button = platform::Button;
using platform::PointerPhase;
using platform::PointerSample;

constexpr double kText = 13.0;
constexpr double kSmall = 11.0;
constexpr double kStatus = 12.0;
constexpr double kMenuRow = 28.0;
constexpr double kMenuSeparator = 9.0;
constexpr double kCategoryRow = 32.0;
constexpr double kItemRow = 40.0;
constexpr double kLayerRow = 40.0;  // §11.1: rows of the layer list
constexpr double kBlendColumn = 124.0;
constexpr double kBlendRow = 26.0;

// Internal menu actions beyond Command and 100 + Tool.
constexpr int kTogglePanels = 200;
constexpr int kToggleDock = 201;
constexpr int kOpenCatalog = 202;
constexpr int kOpenColor = 203;
constexpr int kToolAction = 100;
constexpr int kToggleFlag = 300;  // + LayerFlag: toggles of the active layer
constexpr int kOpenFx = 400;      // + FxKind: the filter dialog
constexpr double kFxWidth = 400.0;
constexpr double kFxRow = 44.0;
constexpr double kFxCurveHeight = 200.0;

constexpr std::wstring_view kCategories[] = {L"Все", L"Перья", L"Аэрограф", L"Акварель", L"Текстура", L"Скринтон", L"Мои"};

/// Fill and the wand use the third slider as their tolerance.
bool toleranceMode(Tool tool) { return tool == Tool::Fill || tool == Tool::Select; }

bool inCategory(const core::BrushProfile& p, int category) {
    using core::BrushFamily;
    switch (category) {
    case 1: return p.family == BrushFamily::Dip || p.family == BrushFamily::Fade || p.family == BrushFamily::Fountain;
    case 2: return p.family == BrushFamily::Airbrush;
    case 3: return p.family == BrushFamily::Watercolor;
    case 4: return p.family == BrushFamily::Texture;
    case 5: return p.family == BrushFamily::Screentone;
    case 6: return p.id.starts_with("user.");  // the artist's library (§14)
    default: return true;
    }
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::optional<double> parseNumber(std::wstring text) {
    std::replace(text.begin(), text.end(), L',', L'.');
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (!end || *end != L'\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

Rect inflate(const Rect& r, double d) { return {r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d}; }

}  // namespace

bool toolAvailable(Tool tool) noexcept {
    (void)tool;
    return true;
}

std::wstring_view toolName(Tool tool) noexcept {
    switch (tool) {
    case Tool::Brush: return L"Кисть";
    case Tool::Eraser: return L"Ластик";
    case Tool::Smudge: return L"Палец";
    case Tool::Blur: return L"Размытие";
    case Tool::Fill: return L"Заливка";
    case Tool::Select: return L"Выделение";
    case Tool::Vector: return L"Векторный узел";
    case Tool::Transform: return L"Трансформация";
    case Tool::Crop: return L"Кадрирование";
    case Tool::Pipette: return L"Пипетка";
    case Tool::Hand: return L"Рука";
    }
    return L"";
}

// ── Construction and layout ──────────────────────────────────────────────────

EditorUi::EditorUi(UiHost& host) : host_(host), theme_(currentTheme()), catalog_(core::builtInBrushes()) {
    const auto cmd = [](Command c) { return static_cast<int>(c); };
    const auto tool = [](Tool t) { return kToolAction + static_cast<int>(t); };
    menus_ = {
        {L"Файл",
         {{L"Новый", L"Ctrl+N", cmd(Command::NewDocument)},
          {L"Открыть…", L"Ctrl+O", cmd(Command::Open)},
          {L"Сохранить", L"Ctrl+S", cmd(Command::Save)},
          {L"Сохранить как…", L"Ctrl+Shift+S", cmd(Command::SaveAs), true},
          {L"Импорт изображения…", L"", cmd(Command::ImportImage)},
          {L"Экспорт изображения…", L"Ctrl+Shift+E", cmd(Command::ExportPng), true},
          {L"Выход", L"Alt+F4", cmd(Command::Exit)}},
         {}},
        {L"Правка",
         {{L"Отменить", L"Ctrl+Z", cmd(Command::Undo)},
          {L"Повторить", L"Ctrl+Y", cmd(Command::Redo), true},
          {L"Вырезать", L"Ctrl+X", cmd(Command::Cut)},
          {L"Копировать", L"Ctrl+C", cmd(Command::Copy)},
          {L"Вставить", L"Ctrl+V", cmd(Command::Paste)}},
         {}},
        {L"Вид",
         {{L"Вписать в окно", L"Ctrl+0", cmd(Command::FitView)},
          {L"Реальный размер", L"Ctrl+1", cmd(Command::ActualPixels), true},
          {L"Скрыть панели", L"Tab", kTogglePanels},
          {L"Док", L"F7", kToggleDock}},
         {}},
        {L"Выделение",
         {{L"Выделить всё", L"Ctrl+A", cmd(Command::SelectAll)},
          {L"Снять выделение", L"Ctrl+D", cmd(Command::Deselect)},
          {L"Инвертировать", L"Ctrl+Shift+I", cmd(Command::InvertSelection), true},
          {L"Выделение", L"M", tool(Tool::Select)},
          {L"Заливка", L"G", tool(Tool::Fill)},
          {L"Перемещение", L"Ctrl+T", tool(Tool::Transform)},
          {L"Кадрирование", L"", tool(Tool::Crop)}},
         {}},
        {L"Слой",
         {{L"Новый слой", L"Ctrl+Shift+N", cmd(Command::NewLayer)},
          {L"Новая группа", L"", cmd(Command::NewGroup)},
          {L"Новый векторный слой", L"", cmd(Command::NewVectorLayer)},
          {L"Растеризовать", L"", cmd(Command::RasterizeLayer)},
          {L"Дублировать", L"", cmd(Command::DuplicateLayer)},
          {L"Удалить", L"", cmd(Command::DeleteLayer), true},
          {L"Выше", L"Alt+↑", cmd(Command::LayerUp)},
          {L"Ниже", L"Alt+↓", cmd(Command::LayerDown), true},
          {L"Обрезать по нижнему", L"", kToggleFlag + static_cast<int>(LayerFlag::ClipToBase)},
          {L"Alpha Lock", L"", kToggleFlag + static_cast<int>(LayerFlag::AlphaLock)},
          {L"Заблокировать", L"", kToggleFlag + static_cast<int>(LayerFlag::Locked)}},
         {}},
        {L"Фильтр",
         {{L"Размытие по Гауссу…", L"", kOpenFx + static_cast<int>(core::FxKind::Gaussian)},
          {L"Кривые…", L"", kOpenFx + static_cast<int>(core::FxKind::ToneCurve)},
          {L"Нерезкая маска…", L"", kOpenFx + static_cast<int>(core::FxKind::Unsharp)},
          {L"Хроматическая аберрация…", L"", kOpenFx + static_cast<int>(core::FxKind::Chromatic)},
          {L"Извлечение линий…", L"", kOpenFx + static_cast<int>(core::FxKind::AnimeLines)}},
         {}},
        {L"Кисть",
         {{L"Каталог кистей…", L"F6", kOpenCatalog},
          {L"Импортировать кисть…", L"", cmd(Command::ImportBrush)},
          {L"Вставить пресет из буфера", L"", cmd(Command::PasteBrush), true},
          {L"Цвет…", L"", kOpenColor, true},
          {L"Кисть", L"B", tool(Tool::Brush)},
          {L"Ластик", L"E", tool(Tool::Eraser)},
          {L"Палец", L"S", tool(Tool::Smudge)},
          {L"Размытие", L"U", tool(Tool::Blur)},
          {L"Пипетка", L"I", tool(Tool::Pipette)},
          {L"Рука", L"Пробел", tool(Tool::Hand)}},
         {}},
        {L"Справка", {{L"О программе", L"", cmd(Command::About)}}, {}},
    };
}

void EditorUi::setCatalog(std::vector<core::BrushProfile> brushes) {
    catalog_ = std::move(brushes);
    if (highlight_ && *highlight_ >= catalog_.size()) highlight_.reset();
}

void EditorUi::resize(double widthDip, double heightDip) {
    input_.width = widthDip;
    input_.height = heightDip;
    relayout();
}

void EditorUi::relayout() {
    input_.dockShown = dockShown_.value_or(defaultDockShown(input_.width));
    layout_ = computeShellLayout(input_);
    input_.toolScroll = layout_.toolScroll;
}

bool EditorUi::overlayOpen() const noexcept { return popup_ != Popup::None || menuOpen_ >= 0; }

Rect EditorUi::popupRect() const {
    const double w = input_.width;
    const double h = input_.height;
    const Rect fallback{layout_.canvas.x + 8, layout_.canvas.y, 0, 0};
    switch (popup_) {
    case Popup::Catalog: {
        const Rect size = catalogSize(w, h);
        return placePopup(layout_.profile.empty() ? fallback : layout_.profile, size.w, size.h, w, h);
    }
    case Popup::Color:
        return placePopup(layout_.color.empty() ? fallback : layout_.color, kColorPopupWidth, kColorPopupHeight, w, h);
    case Popup::More: return placePopup(layout_.more.empty() ? fallback : layout_.more, 232, 56, w, h);
    case Popup::Blend: {
        const Rect anchor = layout_.dockVisible ? propertyRects().blend : fallback;
        return placePopup(anchor, 3 * kBlendColumn + 16, 9 * kBlendRow + 16, w, h);
    }
    case Popup::Fx: {
        const double dh = std::min(fxDialogHeight(), h - 16);
        return {std::max(8.0, std::round((w - kFxWidth) / 2)), std::max(8.0, std::round((h - dh) / 2)), kFxWidth, dh};
    }
    case Popup::None: break;
    }
    return {};
}

Rect EditorUi::menuDropRect(int menu) const {
    if (menu < 0 || menu >= static_cast<int>(menus_.size())) return {};
    const Menu& m = menus_[static_cast<std::size_t>(menu)];
    double height = 8;
    for (const MenuEntry& e : m.entries) height += kMenuRow + (e.separatorAfter ? kMenuSeparator : 0);
    return {m.titleRect.x, 32, 248, height};
}

EditorUi::PickerGeometry EditorUi::picker(const Rect& frame) const {
    PickerGeometry g;
    g.frame = frame;
    g.cx = frame.x + kRingCenterX;
    g.cy = frame.y + kRingCenterY;
    g.square = {frame.x + kSvSquare.x, frame.y + kSvSquare.y, kSvSquare.w, kSvSquare.h};
    const double row1 = frame.y + 216;
    g.fields[0] = {frame.x + 12, row1, 52, 24};
    g.fields[1] = {frame.x + 70, row1, 52, 24};
    g.fields[2] = {frame.x + 128, row1, 52, 24};
    g.fields[3] = {frame.x + 186, row1, 50, 24};
    const double row2 = frame.y + 248;
    g.current = {frame.x + 12, row2, 40, 24};
    g.previous = {frame.x + 52, row2, 40, 24};
    g.fields[4] = {frame.x + 100, row2, 136, 24};
    return g;
}

Rect EditorUi::dockPickerFrame() const {
    const Rect& d = layout_.dock;
    return {d.x + std::max(0.0, (d.w - kColorPopupWidth) / 2), d.y + 44, kColorPopupWidth, kColorPopupHeight};
}

std::vector<EditorUi::LayerRowInfo> EditorUi::layerRows(const UiState& state) const {
    std::vector<LayerRowInfo> rows;
    if (!state.layers) return rows;
    const core::LayerTree& tree = *state.layers;
    // Top of the list is the top of the stack: children from the last to the first.
    const std::function<void(std::uint64_t, int)> walk = [&](std::uint64_t parent, int depth) {
        const std::vector<std::uint64_t>& list = tree.children(parent);
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            rows.push_back({*it, depth});
            const core::LayerNode* n = tree.find(*it);
            if (n && n->kind == core::NodeKind::Group && !collapsed_.contains(*it)) walk(*it, depth + 1);
        }
    };
    walk(0, 0);
    return rows;
}

Rect EditorUi::rowRect(std::size_t row) const {
    const Rect& list = layout_.dockList;
    return {list.x + 4, list.y + 4 + kLayerRow * static_cast<double>(row) - layerScroll_, list.w - 8, kLayerRow - 2};
}

EditorUi::PropertyRects EditorUi::propertyRects() const {
    const Rect& p = layout_.dockProperties;
    PropertyRects r;
    r.opacity = {p.x + 8, p.y + 8, p.w - 16, 32};
    r.blend = {p.x + 8, p.y + 48, std::floor(p.w * 0.42), 32};
    const double rest = p.right() - r.blend.right() - 10;
    r.alphaLock = {r.blend.right() + 10, p.y + 48, std::max(86.0, rest / 2), 32};
    r.clip = {r.alphaLock.right(), p.y + 48, std::max(0.0, p.right() - r.alphaLock.right() - 4), 32};
    return r;
}

Rect EditorUi::blendItemRect(int mode) const {
    const Rect box = popupRect();
    return {box.x + 8 + kBlendColumn * (mode / 9), box.y + 8 + kBlendRow * (mode % 9), kBlendColumn - 4, kBlendRow - 2};
}

const core::LayerNode* EditorUi::activeLayer(const UiState& state) const {
    return state.layers ? state.layers->find(state.layers->active()) : nullptr;
}

EditorUi::DropTarget EditorUi::dropTarget(double y, const UiState& state) const {
    DropTarget t;
    if (!state.layers || dragLayer_ == 0) return t;
    const core::LayerTree& tree = *state.layers;
    const std::vector<LayerRowInfo> rows = layerRows(state);
    if (rows.empty()) return t;
    const double pos = (y - (layout_.dockList.y + 4) + layerScroll_) / kLayerRow;
    const auto clampedRow = static_cast<std::size_t>(std::clamp(std::floor(pos), 0.0, static_cast<double>(rows.size() - 1)));
    const double f = std::clamp(pos - static_cast<double>(clampedRow), 0.0, 1.0);
    const LayerRowInfo& row = rows[clampedRow];
    if (row.id == dragLayer_) return t;
    const core::LayerNode* node = tree.find(row.id);
    if (!node) return t;
    const Rect rr = rowRect(clampedRow);

    // Index among the siblings once the dragged node is taken out of them.
    const auto indexWithout = [&](std::uint64_t parent, std::uint64_t id) {
        std::size_t i = 0;
        for (const std::uint64_t c : tree.children(parent)) {
            if (c == id) return i;
            if (c != dragLayer_) ++i;
        }
        return i;
    };
    const bool group = node->kind == core::NodeKind::Group;
    const bool expanded = group && !collapsed_.contains(row.id) && !node->children.empty();
    if (group && (expanded ? f >= 0.3 : (f >= 0.3 && f <= 0.7))) {
        // Into the group, at its top.
        t.parent = row.id;
        t.index = node->children.size();
        t.into = row.id;
    } else if (f < 0.5) {  // above the row: higher in the stack
        t.parent = node->parent;
        t.index = indexWithout(node->parent, row.id) + 1;
        t.barY = rr.y - 1;
    } else {
        t.parent = node->parent;
        t.index = indexWithout(node->parent, row.id);
        t.barY = rr.bottom() + 1;
    }
    t.barX = rr.x + 32 + 14.0 * row.depth;
    // §7: never into its own subtree.
    t.valid = t.parent == 0 || !tree.isInSubtree(t.parent, dragLayer_);
    return t;
}

void EditorUi::clampLayerScroll(const UiState& state) {
    const double content = 8 + kLayerRow * static_cast<double>(layerRows(state).size());
    layerScroll_ = std::clamp(layerScroll_, 0.0, std::max(0.0, content - layout_.dockList.h));
}

void EditorUi::ensureRowVisible(std::size_t row) {
    const Rect r = rowRect(row);
    const Rect& list = layout_.dockList;
    if (r.y < list.y) layerScroll_ -= list.y - r.y + 4;
    if (r.bottom() > list.bottom()) layerScroll_ += r.bottom() - list.bottom() + 4;
}

void EditorUi::selectRow(int direction, const UiState& state) {
    const std::vector<LayerRowInfo> rows = layerRows(state);
    if (rows.empty() || !state.layers) return;
    std::size_t i = 0;
    while (i < rows.size() && rows[i].id != state.layers->active()) ++i;
    const auto next = i == rows.size() ? std::size_t{0}
                                       : static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
                                             static_cast<std::ptrdiff_t>(i) + direction, 0,
                                             static_cast<std::ptrdiff_t>(rows.size()) - 1));
    host_.selectLayer(rows[next].id);
    ensureRowVisible(next);
    clampLayerScroll(state);
}

void EditorUi::toggleFlag(std::uint64_t id, LayerFlag flag, const UiState& state) {
    const core::LayerNode* n = state.layers ? state.layers->find(id) : nullptr;
    if (!n) return;
    bool on = false;
    switch (flag) {
    case LayerFlag::Visible: on = n->props.visible; break;
    case LayerFlag::AlphaLock: on = n->props.alphaLock; break;
    case LayerFlag::ClipToBase: on = n->props.clipToBase; break;
    case LayerFlag::Locked: on = n->props.locked; break;
    }
    if (flag == LayerFlag::AlphaLock && n->kind != core::NodeKind::Raster) return;
    host_.setLayerFlag(id, flag, !on);
}

Rect EditorUi::layerOpacityRect() const {
    const Rect& p = layout_.dockProperties;
    return {p.x + 8, p.y + 8, p.w - 16, 32};
}

Rect EditorUi::moreSliderRect() const {
    const Rect r = popupRect();
    return {r.x + 8, r.y + 12, r.w - 16, 32};
}

std::vector<std::size_t> EditorUi::catalogItems() const {
    const auto& catalog = catalog_;
    const std::wstring needle = lower(search_);
    std::vector<std::size_t> items;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        if (!inCategory(catalog[i], category_)) continue;
        if (!needle.empty()) {
            const std::wstring hay = lower(widen(catalog[i].name) + L" " + widen(catalog[i].id));
            if (hay.find(needle) == std::wstring::npos) continue;
        }
        items.push_back(i);
    }
    return items;
}

// ── Hit testing ──────────────────────────────────────────────────────────────

EditorUi::HitResult EditorUi::hitTest(double x, double y) const {
    // Overlays first: the open dropdown, then the popup.
    if (menuOpen_ >= 0) {
        const Rect drop = menuDropRect(menuOpen_);
        if (drop.contains(x, y)) {
            double top = drop.y + 4;
            const auto& entries = menus_[static_cast<std::size_t>(menuOpen_)].entries;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (y >= top && y < top + kMenuRow) return {Hit::MenuItem, static_cast<int>(i)};
                top += kMenuRow + (entries[i].separatorAfter ? kMenuSeparator : 0);
            }
            return {Hit::PopupBody};
        }
    }
    if (layout_.menu.contains(x, y)) {
        for (std::size_t i = 0; i < menus_.size(); ++i) {
            if (menus_[i].titleRect.contains(x, y)) return {Hit::MenuTitle, static_cast<int>(i)};
        }
        return {Hit::Chrome};
    }
    if (popup_ != Popup::None) {
        const Rect r = popupRect();
        if (r.contains(x, y)) {
            if (popup_ == Popup::Catalog) {
                const CatalogLayout c = catalogLayout(r);
                if (c.search.contains(x, y)) return {Hit::CatalogSearch};
                if (c.categories.contains(x, y)) {
                    const int i = static_cast<int>((y - c.categories.y) / kCategoryRow);
                    if (i >= 0 && i < static_cast<int>(std::size(kCategories))) return {Hit::CatalogCategory, i};
                }
                if (c.list.contains(x, y)) {
                    const int i = static_cast<int>((y - c.list.y) / kItemRow);
                    if (i >= 0 && i < static_cast<int>(catalogItems().size())) return {Hit::CatalogItem, i};
                }
            } else if (popup_ == Popup::Color) {
                const PickerGeometry g = picker(r);
                const double d = std::hypot(x - g.cx, y - g.cy);
                if (g.square.contains(x, y)) return {Hit::SvSquare};
                if (d >= kRingInner - 2 && d <= kRingOuter + 2) return {Hit::HueRing};
                for (std::size_t i = 0; i < g.fields.size(); ++i) {
                    if (g.fields[i].contains(x, y)) return {Hit::ColorField, static_cast<int>(i)};
                }
                if (g.previous.contains(x, y)) return {Hit::ColorPrevious};
            } else if (popup_ == Popup::Fx) {
                const auto params = fxParams();
                for (std::size_t i = 0; i < params.size(); ++i) {
                    if (fxParamRect(i).contains(x, y)) return {Hit::FxSlider, static_cast<int>(i)};
                }
                if (fx_.kind == core::FxKind::ToneCurve) {
                    if (fxCurveRect().contains(x, y)) return {Hit::FxCurve};
                    for (int c = 0; c < 4; ++c) {
                        if (fxChannelRect(c).contains(x, y)) return {Hit::FxChannel, c};
                    }
                }
                if (!fxCheckRect().empty() && fxCheckRect().contains(x, y)) return {Hit::FxCheck};
                if (fxApplyRect().contains(x, y)) return {Hit::FxApply};
                if (fxCancelRect().contains(x, y)) return {Hit::FxCancel};
            } else if (popup_ == Popup::Blend) {
                for (int m = 0; m < core::kBlendModeCount; ++m) {
                    if (blendItemRect(m).contains(x, y)) return {Hit::BlendItem, m};
                }
            } else if (popup_ == Popup::More) {
                const Rect s = moreSliderRect();
                if (s.contains(x, y)) {
                    return {sliderGeometry(s).value.contains(x, y) ? Hit::StabilizerValue : Hit::Stabilizer};
                }
            }
            return {Hit::PopupBody};
        }
    }

    if (layout_.optionsVisible && layout_.options.contains(x, y)) {
        const auto slider = [&](const Rect& control, Hit track, Hit value) -> std::optional<HitResult> {
            if (!control.contains(x, y)) return std::nullopt;
            return HitResult{sliderGeometry(control).value.contains(x, y) ? value : track};
        };
        if (layout_.profile.contains(x, y)) return {Hit::Profile};
        if (layout_.color.contains(x, y)) return {Hit::Color};
        if (auto h = slider(layout_.size, Hit::Size, Hit::SizeValue)) return *h;
        if (auto h = slider(layout_.opacity, Hit::Opacity, Hit::OpacityValue)) return *h;
        if (auto h = slider(layout_.stabilizer, Hit::Stabilizer, Hit::StabilizerValue)) return *h;
        if (layout_.more.contains(x, y)) return {Hit::More};
        return {Hit::Chrome};
    }
    if (layout_.toolbarVisible && layout_.toolbar.contains(x, y)) {
        for (std::size_t i = 0; i < layout_.tools.size(); ++i) {
            if (layout_.tools[i].contains(x, y)) return {Hit::Tool, static_cast<int>(i)};
        }
        return {Hit::Chrome};
    }
    if (layout_.dockVisible) {
        if (inflate(layout_.splitter, 2).contains(x, y)) return {Hit::Splitter};
        if (layout_.dock.contains(x, y)) {
            if (layout_.dockTabs.contains(x, y)) {
                return {Hit::DockTab, std::clamp(static_cast<int>((x - layout_.dockTabs.x) / (layout_.dockTabs.w / 3)), 0, 2)};
            }
            if (dockTab_ == 0) {
                if (layout_.dockActions.contains(x, y)) {
                    const int i = static_cast<int>((x - layout_.dockActions.x - 8) / 32);
                    if (i >= 0 && i < 5) return {Hit::DockAction, i};
                    return {Hit::Chrome};
                }
                if (layout_.dockList.contains(x, y)) {
                    const double pos = (y - (layout_.dockList.y + 4) + layerScroll_) / kLayerRow;
                    if (pos >= 0) {
                        const auto row = static_cast<std::size_t>(pos);
                        const Rect rr = rowRect(row);
                        if (rr.contains(x, y)) {
                            const int index = static_cast<int>(row);
                            if (x < rr.x + 32) return {Hit::LayerVisibility, index};
                            if (x >= rr.right() - 48 && x < rr.right() - 24) return {Hit::LayerLock, index};
                            if (x >= rr.right() - 24) return {Hit::LayerClip, index};
                            return {Hit::LayerRow, index};  // the chevron is resolved in press()
                        }
                    }
                    return {Hit::LayerList};
                }
                const PropertyRects pr = propertyRects();
                if (pr.opacity.contains(x, y)) {
                    return {sliderGeometry(pr.opacity).value.contains(x, y) ? Hit::LayerOpacityValue : Hit::LayerOpacity};
                }
                if (pr.blend.contains(x, y)) return {Hit::BlendButton};
                if (pr.alphaLock.contains(x, y)) return {Hit::AlphaLockBox};
                if (pr.clip.contains(x, y)) return {Hit::ClipBox};
            } else if (dockTab_ == 1) {
                const PickerGeometry g = picker(dockPickerFrame());
                const double d = std::hypot(x - g.cx, y - g.cy);
                if (g.square.contains(x, y)) return {Hit::SvSquare, 0, true};
                if (d >= kRingInner - 2 && d <= kRingOuter + 2) return {Hit::HueRing, 0, true};
                for (std::size_t i = 0; i < g.fields.size(); ++i) {
                    if (g.fields[i].contains(x, y)) return {Hit::ColorField, static_cast<int>(i), true};
                }
            } else {
                const double top = layout_.dock.y + 40;
                const int i = static_cast<int>((y - top) / kItemRow);
                if (y >= top && i >= 0 && i < static_cast<int>(catalog_.size())) return {Hit::DockBrush, i};
            }
            return {Hit::Chrome};
        }
    }
    if (layout_.status.contains(x, y)) return {Hit::Chrome};
    if (layout_.canvas.contains(x, y)) return {Hit::Canvas};
    return {Hit::None};
}

bool EditorUi::insideOverlay(const HitResult& h) const {
    switch (h.kind) {
    case Hit::MenuItem:
    case Hit::PopupBody:
    case Hit::CatalogSearch:
    case Hit::CatalogCategory:
    case Hit::CatalogItem:
    case Hit::BlendItem:
    case Hit::FxSlider:
    case Hit::FxCurve:
    case Hit::FxChannel:
    case Hit::FxCheck:
    case Hit::FxApply:
    case Hit::FxCancel: return true;
    case Hit::HueRing:
    case Hit::SvSquare:
    case Hit::ColorField:
    case Hit::ColorPrevious: return !h.inDock;
    case Hit::Stabilizer:
    case Hit::StabilizerValue: return popup_ == Popup::More;  // the bar copy is absent while "more" exists
    case Hit::MenuTitle: return menuOpen_ >= 0;
    default: return false;
    }
}

CursorKind EditorUi::cursorAt(double x, double y) const {
    if (drag_ == Drag::Splitter) return CursorKind::ResizeWE;
    if (drag_ == Drag::LayerRow && dragMoved_) return CursorKind::Move;
    const HitResult h = hitTest(x, y);
    switch (h.kind) {
    case Hit::Splitter: return CursorKind::ResizeWE;
    case Hit::SizeValue:
    case Hit::OpacityValue:
    case Hit::StabilizerValue:
    case Hit::LayerOpacityValue:
    case Hit::ColorField:
    case Hit::CatalogSearch: return CursorKind::IBeam;
    case Hit::Canvas: return overlayOpen() ? CursorKind::Arrow : CursorKind::Cross;
    default: return CursorKind::Arrow;
    }
}

// ── Pointer ──────────────────────────────────────────────────────────────────

bool EditorUi::pointer(const PointerSample& s, const UiState& state) {
    const double x = s.x;
    const double y = s.y;
    if (owner_ && owner_->first == s.kind && owner_->second == s.pointerId) {
        switch (s.phase) {
        case PointerPhase::Move: dragTo(x, y, state); break;
        case PointerPhase::Up: endDrag(false, state); break;
        case PointerPhase::Cancel: endDrag(true, state); break;
        default: break;
        }
        return true;
    }
    if (state.painting) return false;  // the canvas owns this contact

    switch (s.phase) {
    case PointerPhase::Hover:
    case PointerPhase::Move: {
        hover_ = hitTest(x, y);
        // While a dropdown is open, hovering another title switches menus.
        if (menuOpen_ >= 0 && hover_.kind == Hit::MenuTitle && hover_.index != menuOpen_) {
            menuOpen_ = hover_.index;
            menuItem_ = -1;
        }
        if (menuOpen_ >= 0 && hover_.kind == Hit::MenuItem) menuItem_ = hover_.index;
        if (popup_ == Popup::Catalog && hover_.kind == Hit::CatalogItem) {
            const auto items = catalogItems();
            highlight_ = items[static_cast<std::size_t>(hover_.index)];  // §11.1: hover changes only the preview
        }
        return false;
    }
    case PointerPhase::Leave:
        hover_ = {};
        return false;
    case PointerPhase::Down: {
        altTap_ = false;
        const HitResult h = hitTest(x, y);
        hover_ = h;
        if (popup_ == Popup::Fx && !insideOverlay(h)) {
            // §11.2 Modal: nothing behind the dialog takes input.
            owner_ = std::pair{s.kind, s.pointerId};
            drag_ = Drag::Swallow;
            return true;
        }
        if (overlayOpen() && !insideOverlay(h)) {
            // §11.2: the first click outside closes the popup and is swallowed.
            if (menuOpen_ >= 0 && h.kind != Hit::MenuTitle) {
                menuOpen_ = -1;
            } else if (popup_ != Popup::None) {
                closePopup(false, state);
            }
            if (h.kind != Hit::MenuTitle) {
                owner_ = std::pair{s.kind, s.pointerId};
                drag_ = Drag::Swallow;
                return true;
            }
        }
        if (h.kind == Hit::Canvas || h.kind == Hit::None) {
            if (editField_ != Field::None) commitEdit(state);
            focus_ = Focus::Canvas;
            return false;
        }
        owner_ = std::pair{s.kind, s.pointerId};
        drag_ = Drag::Swallow;
        pressTime_ = s.timeSeconds;
        if (s.buttons & Button::Primary) press(h, x, y, state);
        return true;
    }
    default: return false;
    }
}

void EditorUi::press(const HitResult& h, double x, double y, const UiState& state) {
    if (editField_ != Field::None) commitEdit(state);
    const auto startSlider = [&](Drag which, Focus focus, const Rect& control, double current) {
        focus_ = focus;
        drag_ = which;
        dragRevert_ = current;
        setSlider(which, sliderT(sliderGeometry(control), x), state);
    };
    switch (h.kind) {
    case Hit::MenuTitle:
        popup_ = Popup::None;
        menuOpen_ = menuOpen_ == h.index ? -1 : h.index;
        menuItem_ = -1;
        break;
    case Hit::MenuItem: {
        const int action = menus_[static_cast<std::size_t>(menuOpen_)].entries[static_cast<std::size_t>(h.index)].action;
        if (!menuEnabled(action, state)) break;
        menuOpen_ = -1;
        runMenuAction(action, state);
        break;
    }
    case Hit::Profile:
        focus_ = Focus::Profile;
        popup_ == Popup::Catalog ? closePopup(false, state) : openPopup(Popup::Catalog, state);
        break;
    case Hit::Color:
        focus_ = Focus::Color;
        popup_ == Popup::Color ? closePopup(false, state) : openPopup(Popup::Color, state);
        break;
    case Hit::More:
        focus_ = Focus::More;
        popup_ == Popup::More ? closePopup(false, state) : openPopup(Popup::More, state);
        break;
    case Hit::Size: startSlider(Drag::Size, Focus::Size, layout_.size, state.diameter); break;
    case Hit::Opacity: startSlider(Drag::Opacity, Focus::Opacity, layout_.opacity, state.opacity); break;
    case Hit::Stabilizer:
        startSlider(Drag::Stabilizer, Focus::Stabilizer, popup_ == Popup::More ? moreSliderRect() : layout_.stabilizer,
                    toleranceMode(state.tool) ? state.tolerance : state.stabilizerMs);
        break;
    case Hit::LayerOpacity:
        if (const core::LayerNode* active = activeLayer(state)) {
            opacityLayer_ = active->id;
            startSlider(Drag::LayerOpacity, Focus::LayerOpacity, layerOpacityRect(), active->props.opacity);
        }
        break;
    case Hit::SizeValue: startEdit(Field::Size, state); break;
    case Hit::OpacityValue: startEdit(Field::Opacity, state); break;
    case Hit::StabilizerValue: startEdit(Field::Stabilizer, state); break;
    case Hit::LayerOpacityValue:
        if (activeLayer(state)) startEdit(Field::LayerOpacity, state);
        break;
    case Hit::Tool: {
        const auto tool = static_cast<Tool>(h.index);
        if (toolAvailable(tool)) host_.selectTool(tool);
        break;
    }
    case Hit::Splitter:
        drag_ = Drag::Splitter;
        dragRevert_ = input_.dockWidth;
        break;
    case Hit::DockTab: dockTab_ = h.index; break;
    case Hit::DockAction: {
        constexpr Command kActions[] = {Command::NewLayer, Command::DuplicateLayer, Command::DeleteLayer, Command::NewGroup};
        if (h.index >= 0 && h.index < 4 && (h.index == 0 || h.index == 3 || activeLayer(state))) {
            host_.run(kActions[h.index]);
        }
        break;
    }
    case Hit::LayerVisibility:
    case Hit::LayerLock:
    case Hit::LayerClip:
    case Hit::LayerRow: {
        const std::vector<LayerRowInfo> rows = layerRows(state);
        if (h.index < 0 || static_cast<std::size_t>(h.index) >= rows.size()) break;
        const LayerRowInfo row = rows[static_cast<std::size_t>(h.index)];
        const core::LayerNode* node = state.layers->find(row.id);
        if (!node) break;
        focus_ = Focus::LayerList;
        if (h.kind == Hit::LayerVisibility) {
            toggleFlag(row.id, LayerFlag::Visible, state);
            break;
        }
        if (h.kind == Hit::LayerLock) {
            toggleFlag(row.id, LayerFlag::Locked, state);
            break;
        }
        if (h.kind == Hit::LayerClip) {
            toggleFlag(row.id, LayerFlag::ClipToBase, state);
            break;
        }
        const Rect rr = rowRect(static_cast<std::size_t>(h.index));
        const double chevron = rr.x + 32 + 14.0 * row.depth;
        if (node->kind == core::NodeKind::Group && x >= chevron && x < chevron + 16) {
            if (!collapsed_.erase(row.id)) collapsed_.insert(row.id);
            clampLayerScroll(state);
            break;
        }
        // A second press on the same row within the double-click time renames it (§11.1).
        const double doubleClick = GetDoubleClickTime() / 1000.0;
        if (lastClickLayer_ == row.id && pressTime_ - lastClickTime_ <= doubleClick && x >= chevron + 50) {
            lastClickLayer_ = 0;
            renameId_ = row.id;
            startEdit(Field::LayerName, state);
            break;
        }
        lastClickLayer_ = row.id;
        lastClickTime_ = pressTime_;
        if (state.layers->active() != row.id) host_.selectLayer(row.id);
        // Reordering starts after 4 DIP of movement (§11.2).
        drag_ = Drag::LayerRow;
        dragLayer_ = row.id;
        dragStartY_ = y;
        dragY_ = y;
        dragMoved_ = false;
        break;
    }
    case Hit::BlendButton:
        if (const core::LayerNode* active = activeLayer(state)) {
            focus_ = Focus::Blend;
            blendHighlight_ = active->props.blendId;
            popup_ == Popup::Blend ? closePopup(false, state) : openPopup(Popup::Blend, state);
        }
        break;
    case Hit::AlphaLockBox:
        if (const core::LayerNode* active = activeLayer(state)) toggleFlag(active->id, LayerFlag::AlphaLock, state);
        break;
    case Hit::ClipBox:
        if (const core::LayerNode* active = activeLayer(state)) toggleFlag(active->id, LayerFlag::ClipToBase, state);
        break;
    case Hit::BlendItem:
        if (const core::LayerNode* active = activeLayer(state)) host_.setLayerBlend(active->id, h.index);
        closePopup(false, state);
        break;
    case Hit::FxSlider:
        fxFocus_ = h.index;
        fxSlider_ = h.index;
        drag_ = Drag::FxSlider;
        dragTo(x, y, state);
        break;
    case Hit::FxCurve:
        drag_ = Drag::FxCurve;
        fxCurvePress(x, y);
        break;
    case Hit::FxChannel:
        fxChannel_ = h.index;
        break;
    case Hit::FxCheck:
        if (fx_.kind == core::FxKind::ToneCurve) fx_.curve.discrete = !fx_.curve.discrete;
        if (fx_.kind == core::FxKind::AnimeLines) fx_.toNewLayer = !fx_.toNewLayer;
        host_.fxChanged(fx_);
        break;
    case Hit::FxApply: closeFx(true); break;
    case Hit::FxCancel: closeFx(false); break;
    case Hit::CatalogSearch: focus_ = Focus::Search; break;
    case Hit::CatalogCategory:
        category_ = h.index;
        focus_ = Focus::Categories;
        break;
    case Hit::CatalogItem: {
        const auto items = catalogItems();
        host_.selectBrush(items[static_cast<std::size_t>(h.index)]);
        closePopup(false, state);
        break;
    }
    case Hit::DockBrush: host_.selectBrush(static_cast<std::size_t>(h.index)); break;
    case Hit::HueRing:
    case Hit::SvSquare: {
        drag_ = h.kind == Hit::HueRing ? Drag::Hue : Drag::Sv;
        dragInDock_ = h.inDock;
        colorRevert_ = state.color;
        alphaRevert_ = state.alpha;
        if (h.inDock) syncHsv(state);
        dragTo(x, y, state);
        break;
    }
    case Hit::ColorField: {
        constexpr Field fields[] = {Field::H, Field::S, Field::V, Field::A, Field::Hex};
        startEdit(fields[static_cast<std::size_t>(h.index)], state);
        break;
    }
    case Hit::ColorPrevious:
        host_.setColor(colorAtOpen_, alphaAtOpen_);
        hsv_ = core::rgbToHsv(colorAtOpen_, lastHue_);
        break;
    default: break;
    }
}

void EditorUi::setSlider(Drag which, double t, const UiState& state) {
    lastDragValue_ = t;
    switch (which) {
    case Drag::Size: host_.setDiameter(diameterFromT(t)); break;
    case Drag::Opacity: host_.setOpacity(t); break;
    case Drag::Stabilizer:
        if (toleranceMode(state.tool)) {
            host_.setTolerance(std::round(t * 100.0) / 100.0);
        } else {
            host_.setStabilizer(std::round(t * 200.0));
        }
        break;
    case Drag::LayerOpacity: host_.setLayerOpacity(opacityLayer_, t, false); break;
    default: break;
    }
}

void EditorUi::dragTo(double x, double y, const UiState& state) {
    switch (drag_) {
    case Drag::Size: setSlider(drag_, sliderT(sliderGeometry(layout_.size), x), state); break;
    case Drag::Opacity: setSlider(drag_, sliderT(sliderGeometry(layout_.opacity), x), state); break;
    case Drag::Stabilizer:
        setSlider(drag_, sliderT(sliderGeometry(popup_ == Popup::More ? moreSliderRect() : layout_.stabilizer), x), state);
        break;
    case Drag::LayerOpacity: setSlider(drag_, sliderT(sliderGeometry(layerOpacityRect()), x), state); break;
    case Drag::LayerRow:
        dragY_ = y;
        if (!dragMoved_ && std::abs(y - dragStartY_) >= 4.0) dragMoved_ = true;
        break;
    case Drag::FxSlider: {
        const auto params = fxParams();
        if (fxSlider_ < 0 || static_cast<std::size_t>(fxSlider_) >= params.size()) break;
        const FxParamInfo& p = params[static_cast<std::size_t>(fxSlider_)];
        double v = p.min + sliderT(sliderGeometry(fxParamRect(static_cast<std::size_t>(fxSlider_))), x) * (p.max - p.min);
        if (p.integer) v = std::round(v);
        if (v != fxValue(p.id)) {
            setFxValue(p.id, v);
            host_.fxChanged(fx_);
        }
        break;
    }
    case Drag::FxCurve: fxCurveDrag(x, y); break;
    case Drag::Splitter:
        input_.dockWidth = std::clamp(input_.width - x - 2, kMinDockWidth, kMaxDockWidth);
        relayout();
        break;
    case Drag::Hue: {
        const PickerGeometry g = picker(dragInDock_ ? dockPickerFrame() : popupRect());
        hsv_.h = core::hueFromOffset(x - g.cx, y - g.cy);
        lastHue_ = hsv_.h;
        applyHsv(state);
        break;
    }
    case Drag::Sv: {
        const PickerGeometry g = picker(dragInDock_ ? dockPickerFrame() : popupRect());
        hsv_.s = std::clamp((x - g.square.x) / g.square.w, 0.0, 1.0);
        hsv_.v = 1.0 - std::clamp((y - g.square.y) / g.square.h, 0.0, 1.0);
        applyHsv(state);
        break;
    }
    default: break;
    }
}

void EditorUi::endDrag(bool cancelled, const UiState& state) {
    if (cancelled) {
        // §11.2: Esc or an unexpected loss returns the snapshot.
        switch (drag_) {
        case Drag::Size: host_.setDiameter(dragRevert_); break;
        case Drag::Opacity: host_.setOpacity(dragRevert_); break;
        case Drag::Stabilizer:
            if (toleranceMode(state.tool)) {
                host_.setTolerance(dragRevert_);
            } else {
                host_.setStabilizer(dragRevert_);
            }
            break;
        case Drag::LayerOpacity: host_.setLayerOpacity(opacityLayer_, dragRevert_, false); break;
        case Drag::Splitter:
            input_.dockWidth = dragRevert_;
            relayout();
            break;
        case Drag::Hue:
        case Drag::Sv:
            host_.setColor(colorRevert_, alphaRevert_);
            hsv_ = core::rgbToHsv(colorRevert_, lastHue_);
            break;
        default: break;
        }
    } else if (drag_ == Drag::LayerOpacity) {
        host_.setLayerOpacity(opacityLayer_, lastDragValue_, true);  // one history command per drag
    } else if (drag_ == Drag::LayerRow && dragMoved_) {
        const DropTarget target = dropTarget(dragY_, state);
        if (target.valid) host_.moveLayer(dragLayer_, target.parent, target.index);
    }
    if (drag_ == Drag::LayerRow) {
        dragLayer_ = 0;
        dragMoved_ = false;
    }
    fxSlider_ = -1;
    fxPoint_ = -1;
    (void)state;
    drag_ = Drag::None;
    owner_.reset();
}

void EditorUi::abandon(const UiState& state) {
    if (drag_ != Drag::None) endDrag(true, state);
    if (editField_ != Field::None) cancelEdit();
    menuOpen_ = -1;
    if (popup_ != Popup::None && popup_ != Popup::Fx) closePopup(false, state);  // the modal dialog stays
    altTap_ = false;
}

// ── Popups, editing, focus ───────────────────────────────────────────────────

void EditorUi::syncHsv(const UiState& state) {
    hsv_ = core::rgbToHsv(state.color, lastHue_);
    if (hsv_.s > 0.0) lastHue_ = hsv_.h;
}

void EditorUi::applyHsv(const UiState& state) {
    if (hsv_.s > 0.0) lastHue_ = hsv_.h;
    host_.setColor(core::hsvToRgb(hsv_), state.alpha);
}

void EditorUi::openPopup(Popup popup, const UiState& state) {
    if (editField_ != Field::None) commitEdit(state);
    menuOpen_ = -1;
    popup_ = popup;
    if (popup == Popup::Color) {
        colorAtOpen_ = state.color;
        alphaAtOpen_ = state.alpha;
        syncHsv(state);
    } else if (popup == Popup::Catalog) {
        highlight_ = state.brush;
        focus_ = Focus::Search;
    }
}

void EditorUi::closePopup(bool restore, const UiState& state) {
    if (editField_ != Field::None) cancelEdit();
    if (restore && popup_ == Popup::Color) host_.setColor(colorAtOpen_, alphaAtOpen_);  // §11.1: Esc restores
    // §11.1 / §11.2: focus returns to the anchor.
    switch (popup_) {
    case Popup::Catalog: focus_ = Focus::Profile; break;
    case Popup::Color: focus_ = Focus::Color; break;
    case Popup::More: focus_ = Focus::More; break;
    case Popup::Blend: focus_ = Focus::Blend; break;
    case Popup::None: break;
    }
    if (layout_.profile.empty()) focus_ = Focus::Canvas;  // panels hidden: no anchor
    popup_ = Popup::None;
    highlight_.reset();
    (void)state;
}

std::wstring EditorUi::fieldText(Field f, const UiState& state) const {
    switch (f) {
    case Field::Size: return std::format(L"{:.1f}", state.diameter);
    case Field::Opacity: return std::format(L"{:.0f}", state.opacity * 100);
    case Field::Stabilizer:
        return toleranceMode(state.tool) ? std::format(L"{:.0f}", state.tolerance * 100) : std::format(L"{:.0f}", state.stabilizerMs);
    case Field::LayerOpacity: {
        const core::LayerNode* n = state.layers ? state.layers->find(opacityLayer_) : nullptr;
        if (!n) n = activeLayer(state);
        return n ? std::format(L"{:.0f}", n->props.opacity * 100) : std::wstring{};
    }
    case Field::LayerName: {
        const core::LayerNode* n = state.layers ? state.layers->find(renameId_) : nullptr;
        return n ? widen(n->props.name) : std::wstring{};
    }
    case Field::H: return std::format(L"{:.0f}", hsv_.h * 360);
    case Field::S: return std::format(L"{:.0f}", hsv_.s * 100);
    case Field::V: return std::format(L"{:.0f}", hsv_.v * 100);
    case Field::A: return std::format(L"{:.0f}", state.alpha * 100);
    case Field::Hex: return widen(core::toHex(state.color));
    default: return {};
    }
}

void EditorUi::startEdit(Field field, const UiState& state) {
    editField_ = field;
    editText_ = fieldText(field, state);
    editFresh_ = true;
    switch (field) {
    case Field::Size: focus_ = Focus::Size; break;
    case Field::Opacity: focus_ = Focus::Opacity; break;
    case Field::Stabilizer: focus_ = Focus::Stabilizer; break;
    case Field::LayerOpacity:
        focus_ = Focus::LayerOpacity;
        if (const core::LayerNode* active = activeLayer(state)) opacityLayer_ = active->id;
        break;
    case Field::LayerName: focus_ = Focus::LayerList; break;
    case Field::H: focus_ = Focus::H; break;
    case Field::S: focus_ = Focus::S; break;
    case Field::V: focus_ = Focus::V; break;
    case Field::A: focus_ = Focus::A; break;
    case Field::Hex: focus_ = Focus::Hex; break;
    default: break;
    }
}

// §11.1: a typed number + Enter applies with clamping; Esc returns the value before editing.
void EditorUi::commitEdit(const UiState& state) {
    const Field field = editField_;
    editField_ = Field::None;
    if (field == Field::Hex) {
        std::string ascii;
        for (const wchar_t c : editText_) ascii.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        if (const auto rgb = core::parseHex(ascii)) {
            host_.setColor(*rgb, state.alpha);
            hsv_ = core::rgbToHsv(*rgb, lastHue_);
        }
        return;
    }
    if (field == Field::LayerName) {
        // Trimmed; an empty name keeps the old one.
        const auto first = editText_.find_first_not_of(L" \t");
        const auto last = editText_.find_last_not_of(L" \t");
        if (first != std::wstring::npos && renameId_ != 0) {
            host_.renameLayer(renameId_, narrow(std::wstring_view(editText_).substr(first, last - first + 1)));
        }
        renameId_ = 0;
        return;
    }
    const auto value = parseNumber(editText_);
    if (!value) return;
    switch (field) {
    case Field::Size: host_.setDiameter(std::clamp(*value, 0.25, 2048.0)); break;
    case Field::Opacity: host_.setOpacity(std::clamp(*value, 0.0, 100.0) / 100); break;
    case Field::Stabilizer:
        if (toleranceMode(state.tool)) {
            host_.setTolerance(std::clamp(*value, 0.0, 100.0) / 100);
        } else {
            host_.setStabilizer(std::clamp(*value, 0.0, 200.0));
        }
        break;
    case Field::LayerOpacity: host_.setLayerOpacity(opacityLayer_, std::clamp(*value, 0.0, 100.0) / 100, true); break;
    case Field::H:
        hsv_.h = std::fmod(std::fmod(*value, 360.0) + 360.0, 360.0) / 360.0;
        lastHue_ = hsv_.h;
        applyHsv(state);
        break;
    case Field::S:
        hsv_.s = std::clamp(*value, 0.0, 100.0) / 100;
        applyHsv(state);
        break;
    case Field::V:
        hsv_.v = std::clamp(*value, 0.0, 100.0) / 100;
        applyHsv(state);
        break;
    case Field::A: host_.setColor(state.color, std::clamp(*value, 0.0, 100.0) / 100); break;
    default: break;
    }
}

void EditorUi::cancelEdit() {
    editField_ = Field::None;
    renameId_ = 0;
}

void EditorUi::moveFocus(bool backwards, const UiState& state) {
    std::vector<Focus> order;
    if (popup_ == Popup::Catalog) {
        order = {Focus::Search, Focus::Categories, Focus::Items};
    } else if (popup_ == Popup::Color) {
        order = {Focus::H, Focus::S, Focus::V, Focus::A, Focus::Hex};
    } else if (popup_ == Popup::Blend) {
        order = {Focus::BlendItems};
    } else {
        if (!layout_.profile.empty()) order = {Focus::Profile, Focus::Color, Focus::Size, Focus::Opacity};
        if (!layout_.stabilizer.empty()) order.push_back(Focus::Stabilizer);
        if (!layout_.more.empty()) order.push_back(Focus::More);
        if (layout_.dockVisible && dockTab_ == 0) {
            order.push_back(Focus::LayerList);
            order.push_back(Focus::LayerOpacity);
            order.push_back(Focus::Blend);
        }
    }
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), focus_);
    std::size_t index = it == order.end() ? 0 : static_cast<std::size_t>(it - order.begin());
    if (it != order.end()) index = backwards ? (index + order.size() - 1) % order.size() : (index + 1) % order.size();
    focus_ = order[index];
    // In the colour popup, focusing a field starts editing it.
    if (popup_ == Popup::Color) {
        constexpr std::pair<Focus, Field> map[] = {
            {Focus::H, Field::H}, {Focus::S, Field::S}, {Focus::V, Field::V}, {Focus::A, Field::A}, {Focus::Hex, Field::Hex}};
        for (const auto& [f, field] : map) {
            if (f == focus_) startEdit(field, state);
        }
    }
}

void EditorUi::stepFocused(int direction, bool big, const UiState& state) {
    const double k = big ? 10.0 : 1.0;  // §11.1: Shift = 10 steps
    switch (focus_) {
    case Focus::Size: host_.setDiameter(std::clamp(state.diameter + direction * 0.25 * k, 0.25, 2048.0)); break;
    case Focus::Opacity: host_.setOpacity(std::clamp(state.opacity + direction * 0.01 * k, 0.0, 1.0)); break;
    case Focus::Stabilizer:
        if (toleranceMode(state.tool)) {
            host_.setTolerance(std::clamp(state.tolerance + direction * 0.01 * k, 0.0, 1.0));
        } else {
            host_.setStabilizer(std::clamp(state.stabilizerMs + direction * k, 0.0, 200.0));
        }
        break;
    case Focus::LayerOpacity:
        if (const core::LayerNode* active = activeLayer(state)) {
            host_.setLayerOpacity(active->id, std::clamp(active->props.opacity + direction * 0.01 * k, 0.0, 1.0), true);
        }
        break;
    case Focus::LayerList: selectRow(-direction, state); break;  // Up (+1) goes up the list
    case Focus::BlendItems:
        blendHighlight_ = std::clamp(blendHighlight_ + direction, 0, core::kBlendModeCount - 1);
        break;
    case Focus::Categories:
        category_ = std::clamp(category_ + direction, 0, static_cast<int>(std::size(kCategories)) - 1);
        break;
    case Focus::Search:
    case Focus::Items: {
        const auto items = catalogItems();
        if (items.empty()) break;
        auto it = highlight_ ? std::find(items.begin(), items.end(), *highlight_) : items.end();
        std::ptrdiff_t i = it == items.end() ? 0 : (it - items.begin()) + direction;
        i = std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(items.size()) - 1);
        highlight_ = items[static_cast<std::size_t>(i)];
        focus_ = Focus::Items;
        break;
    }
    default: break;
    }
}

void EditorUi::activateFocused(const UiState& state) {
    switch (focus_) {
    case Focus::Profile: popup_ == Popup::Catalog ? closePopup(false, state) : openPopup(Popup::Catalog, state); break;
    case Focus::Color: popup_ == Popup::Color ? closePopup(false, state) : openPopup(Popup::Color, state); break;
    case Focus::More: popup_ == Popup::More ? closePopup(false, state) : openPopup(Popup::More, state); break;
    case Focus::Size: startEdit(Field::Size, state); break;
    case Focus::Opacity: startEdit(Field::Opacity, state); break;
    case Focus::Stabilizer: startEdit(Field::Stabilizer, state); break;
    case Focus::LayerOpacity:
        if (activeLayer(state)) startEdit(Field::LayerOpacity, state);
        break;
    case Focus::LayerList:
        if (const core::LayerNode* active = activeLayer(state)) {
            renameId_ = active->id;
            startEdit(Field::LayerName, state);
        }
        break;
    case Focus::Blend:
        if (const core::LayerNode* active = activeLayer(state)) {
            blendHighlight_ = active->props.blendId;
            openPopup(Popup::Blend, state);
            focus_ = Focus::BlendItems;
        }
        break;
    case Focus::BlendItems:
        if (const core::LayerNode* active = activeLayer(state)) host_.setLayerBlend(active->id, blendHighlight_);
        closePopup(false, state);
        break;
    case Focus::Categories: focus_ = Focus::Items; break;
    case Focus::Search:
    case Focus::Items:
        // §11.1: Enter selects the preset.
        if (highlight_) {
            host_.selectBrush(*highlight_);
            closePopup(false, state);
        }
        break;
    default: break;
    }
}

bool EditorUi::menuEnabled(int action, const UiState& state) const {
    if (action == static_cast<int>(Command::Undo)) return state.canUndo;
    if (action == static_cast<int>(Command::Redo)) return state.canRedo;
    const core::LayerNode* active = activeLayer(state);
    if (action >= kToggleFlag && action < kToggleFlag + 4) {
        return active && (action != kToggleFlag + static_cast<int>(LayerFlag::AlphaLock) || active->kind == core::NodeKind::Raster);
    }
    switch (action) {
    case static_cast<int>(Command::DuplicateLayer):
    case static_cast<int>(Command::DeleteLayer):
    case static_cast<int>(Command::LayerUp):
    case static_cast<int>(Command::LayerDown): return active != nullptr;
    default: break;
    }
    if (action >= kToolAction && action < kToolAction + kToolCount) return toolAvailable(static_cast<Tool>(action - kToolAction));
    return true;
}

void EditorUi::runMenuAction(int action, const UiState& state) {
    if (action >= kToolAction && action < kToolAction + kToolCount) {
        host_.selectTool(static_cast<Tool>(action - kToolAction));
        return;
    }
    if (action >= kToggleFlag && action < kToggleFlag + 4) {
        if (const core::LayerNode* active = activeLayer(state)) {
            toggleFlag(active->id, static_cast<LayerFlag>(action - kToggleFlag), state);
        }
        return;
    }
    if (action >= kOpenFx && action < kOpenFx + core::kFxCount) {
        openFx(static_cast<core::FxKind>(action - kOpenFx));
        return;
    }
    switch (action) {
    case kTogglePanels:
        input_.panelsHidden = !input_.panelsHidden;
        relayout();
        return;
    case kToggleDock:
        dockShown_ = !layout_.dockVisible;
        relayout();
        return;
    case kOpenCatalog: openPopup(Popup::Catalog, state); return;
    case kOpenColor: openPopup(Popup::Color, state); return;
    default: host_.run(static_cast<Command>(action)); return;
    }
}

// ── Keyboard ─────────────────────────────────────────────────────────────────

bool EditorUi::key(const platform::KeyEvent& k, const UiState& state) {
    if (popup_ == Popup::Fx) {
        // §11.2 Modal: Enter runs the main command, Esc cancels; everything else stays in the dialog.
        if (!k.down) return true;
        const auto params = fxParams();
        switch (k.virtualKey) {
        case VK_ESCAPE: closeFx(false); break;
        case VK_RETURN: closeFx(true); break;
        case VK_TAB:
        case VK_DOWN:
        case VK_UP: {
            const int n = static_cast<int>(params.size());
            const bool back = k.virtualKey == VK_UP || (k.virtualKey == VK_TAB && k.shift);
            if (n > 0) fxFocus_ = (fxFocus_ + (back ? n - 1 : 1)) % n;
            break;
        }
        case VK_LEFT:
        case VK_RIGHT:
            if (fxFocus_ >= 0 && static_cast<std::size_t>(fxFocus_) < params.size()) {
                const FxParamInfo& p = params[static_cast<std::size_t>(fxFocus_)];
                const double step = (p.integer ? 1.0 : (p.max - p.min) / 100.0) * (k.shift ? 10.0 : 1.0);
                setFxValue(p.id, std::clamp(fxValue(p.id) + (k.virtualKey == VK_LEFT ? -step : step), p.min, p.max));
                host_.fxChanged(fx_);
            }
            break;
        default: break;
        }
        return true;
    }
    // Alt pressed and released alone activates the menu (and keeps Alt+F4, Alt+Space for the system).
    if (k.virtualKey == VK_MENU) {
        if (k.down && !k.repeat) altTap_ = true;
        if (!k.down && altTap_) {
            altTap_ = false;
            if (menuOpen_ >= 0) {
                menuOpen_ = -1;
            } else {
                popup_ = Popup::None;
                menuOpen_ = 0;
                menuItem_ = 0;
            }
            return true;
        }
        return false;
    }
    if (k.down) altTap_ = false;
    if (!k.down) return editField_ != Field::None;

    // Text entry swallows every key (spec §11.2: tool shortcuts are suppressed while typing).
    if (editField_ != Field::None) {
        switch (k.virtualKey) {
        case VK_RETURN: commitEdit(state); break;
        case VK_ESCAPE: cancelEdit(); break;
        case VK_TAB:
            commitEdit(state);
            moveFocus(k.shift, state);
            break;
        case VK_BACK:
            if (editFresh_) editText_.clear();
            if (!editText_.empty()) editText_.pop_back();
            editFresh_ = false;
            break;
        default: break;
        }
        return true;
    }
    if (k.virtualKey == VK_ESCAPE && drag_ == Drag::LayerRow) {
        // §11.2: Esc cancels the reorder; the rest of the contact is swallowed.
        drag_ = Drag::Swallow;
        dragLayer_ = 0;
        dragMoved_ = false;
        return true;
    }
    if (k.virtualKey == VK_ESCAPE && drag_ != Drag::None && drag_ != Drag::Swallow) {
        // §11.2: Esc restores the snapshot; the rest of the contact is swallowed until Up.
        const auto owner = owner_;
        endDrag(true, state);
        owner_ = owner;
        drag_ = owner_ ? Drag::Swallow : Drag::None;
        return true;
    }

    // Menu navigation.
    if (menuOpen_ >= 0) {
        const auto& entries = menus_[static_cast<std::size_t>(menuOpen_)].entries;
        const int count = static_cast<int>(entries.size());
        switch (k.virtualKey) {
        case VK_ESCAPE:
        case VK_F10: menuOpen_ = -1; break;
        case VK_LEFT: menuOpen_ = (menuOpen_ + static_cast<int>(menus_.size()) - 1) % static_cast<int>(menus_.size()); menuItem_ = 0; break;
        case VK_RIGHT: menuOpen_ = (menuOpen_ + 1) % static_cast<int>(menus_.size()); menuItem_ = 0; break;
        case VK_UP: menuItem_ = (std::max(menuItem_, 0) + count - 1) % count; break;
        case VK_DOWN: menuItem_ = (menuItem_ + 1) % count; break;
        case VK_RETURN:
        case VK_SPACE:
            if (menuItem_ >= 0 && menuItem_ < count) {
                const int action = entries[static_cast<std::size_t>(menuItem_)].action;
                if (menuEnabled(action, state)) {
                    menuOpen_ = -1;
                    runMenuAction(action, state);
                }
            }
            break;
        default: return k.ctrl;  // let Ctrl shortcuts through, swallow the rest
        }
        return true;
    }

    switch (k.virtualKey) {
    case VK_F10:  // spec §11.2: F10 activates the menu
        popup_ = Popup::None;
        menuOpen_ = 0;
        menuItem_ = 0;
        return true;
    case VK_F6:  // brush catalogue
        popup_ == Popup::Catalog ? closePopup(false, state) : openPopup(Popup::Catalog, state);
        return true;
    case VK_F7:
        if (k.ctrl) return false;  // Ctrl+F7: the layer manager belongs to the layer phase
        dockShown_ = !layout_.dockVisible;
        relayout();
        return true;
    case VK_TAB:
        if (popup_ != Popup::None || focus_ != Focus::Canvas) {
            moveFocus(k.shift, state);
        } else {
            // §11.1: Tab with the focus on the canvas hides the panels until pressed again.
            input_.panelsHidden = !input_.panelsHidden;
            relayout();
        }
        return true;
    default: break;
    }

    if (popup_ == Popup::Blend) {
        switch (k.virtualKey) {
        case VK_ESCAPE: closePopup(true, state); return true;
        case VK_RETURN:
        case VK_SPACE:
            focus_ = Focus::BlendItems;
            activateFocused(state);
            return true;
        case VK_UP: blendHighlight_ = std::max(0, blendHighlight_ - 1); return true;
        case VK_DOWN: blendHighlight_ = std::min(core::kBlendModeCount - 1, blendHighlight_ + 1); return true;
        case VK_LEFT: blendHighlight_ = std::max(0, blendHighlight_ - 9); return true;
        case VK_RIGHT: blendHighlight_ = std::min(core::kBlendModeCount - 1, blendHighlight_ + 9); return true;
        default: return !k.ctrl;
        }
    }
    if (popup_ != Popup::None) {
        switch (k.virtualKey) {
        case VK_ESCAPE: closePopup(true, state); return true;
        case VK_RETURN: activateFocused(state); return true;
        case VK_UP: stepFocused(-1, k.shift, state); return true;
        case VK_DOWN: stepFocused(1, k.shift, state); return true;
        case VK_BACK:
            if (popup_ == Popup::Catalog && focus_ == Focus::Search && !search_.empty()) {
                const bool pair = search_.size() >= 2 && search_.back() >= 0xDC00 && search_.back() <= 0xDFFF;
                search_.pop_back();
                if (pair) search_.pop_back();  // the high surrogate of the same code point
                // Keep the highlight when it still matches, else the first match.
                const auto items = catalogItems();
                if (!highlight_ || std::find(items.begin(), items.end(), *highlight_) == items.end()) {
                    highlight_ = items.empty() ? std::optional<std::size_t>{} : std::optional<std::size_t>{items.front()};
                }
            }
            return true;
        default: return !k.ctrl;  // shortcuts stay available, plain keys belong to the popup
        }
    }
    if (focus_ == Focus::LayerList && !k.ctrl && !k.alt) {
        const core::LayerNode* active = activeLayer(state);
        switch (k.virtualKey) {
        case VK_UP: selectRow(-1, state); return true;
        case VK_DOWN: selectRow(1, state); return true;
        case VK_LEFT:
        case VK_RIGHT:
            if (active && active->kind == core::NodeKind::Group) {
                if (k.virtualKey == VK_LEFT) {
                    collapsed_.insert(active->id);
                } else {
                    collapsed_.erase(active->id);
                }
                clampLayerScroll(state);
            }
            return true;
        case VK_SPACE:
            if (active) toggleFlag(active->id, LayerFlag::Visible, state);
            return true;
        case VK_RETURN:
        case VK_F2: activateFocused(state); return true;
        default: break;
        }
    }
    if (focus_ != Focus::Canvas) {
        if (k.alt) return false;  // Alt+↑ / Alt+↓ move the layer (the host's), never the focus
        switch (k.virtualKey) {
        case VK_ESCAPE: focus_ = Focus::Canvas; return true;
        case VK_LEFT:
        case VK_DOWN: stepFocused(-1, k.shift, state); return true;
        case VK_RIGHT:
        case VK_UP: stepFocused(1, k.shift, state); return true;
        case VK_RETURN:
        case VK_SPACE: activateFocused(state); return true;
        default: return false;
        }
    }
    return false;
}

bool EditorUi::character(char32_t c, const UiState& state) {
    (void)state;
    if (c < 0x20 || c == 0x7F) return editField_ != Field::None;  // control characters come as keys
    if (editField_ == Field::LayerName) {
        if (editFresh_) editText_.clear();
        editFresh_ = false;
        if (editText_.size() < 64) {
            if (c >= 0x10000) {
                const char32_t v = c - 0x10000;
                editText_.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
                editText_.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            } else {
                editText_.push_back(static_cast<wchar_t>(c));
            }
        }
        return true;
    }
    if (editField_ != Field::None) {
        const bool hex = editField_ == Field::Hex;
        const bool ok = (c >= U'0' && c <= U'9') || c == U'.' || c == U',' || c == U'-' ||
                        (hex && (c == U'#' || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F')));
        if (!ok) return true;
        if (editFresh_) editText_.clear();
        editFresh_ = false;
        if (editText_.size() < 12) editText_.push_back(static_cast<wchar_t>(c));
        return true;
    }
    if (popup_ == Popup::Catalog && (focus_ == Focus::Search || focus_ == Focus::Items || focus_ == Focus::Categories)) {
        focus_ = Focus::Search;
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            search_.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            search_.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        } else {
            search_.push_back(static_cast<wchar_t>(c));
        }
        const auto items = catalogItems();
        highlight_ = items.empty() ? std::optional<std::size_t>{} : std::optional<std::size_t>{items.front()};
        return true;
    }
    return false;
}

bool EditorUi::wheel(const platform::WheelEvent& w, const UiState& state) {
    if (layout_.dockVisible && dockTab_ == 0 && layout_.dockList.contains(w.x, w.y) && !overlayOpen()) {
        layerScroll_ -= w.notches * kLayerRow;
        clampLayerScroll(state);
        return true;
    }
    if (layout_.toolbarVisible && layout_.toolbar.contains(w.x, w.y)) {
        input_.toolScroll -= w.notches * 44.0;
        relayout();
        return true;
    }
    if (overlayOpen()) return true;
    return false;
}

// ── Help ─────────────────────────────────────────────────────────────────────

std::wstring EditorUi::helpFor(const HitResult& h, const UiState& state) const {
    const auto& catalog = catalog_;
    switch (h.kind) {
    case Hit::Tool: {
        const auto tool = static_cast<Tool>(h.index);
        switch (tool) {
        case Tool::Brush: return L"Кисть (B)";
        case Tool::Eraser: return L"Ластик (E); обратный конец пера тоже стирает";
        case Tool::Pipette: return L"Пипетка (I): берёт цвет с холста";
        case Tool::Hand: return L"Рука: сдвиг вида; также пробел + перетаскивание или средняя кнопка";
        case Tool::Smudge: return L"Палец (S): тянет краску слоя формой текущей кисти";
        case Tool::Blur: return L"Размытие (U): размывает слой под кистью";
        case Tool::Vector: return L"Векторный узел (V): узлы и ручки объекта, ромб — толщина, двойной щелчок — новый узел, Delete";
        case Tool::Select: return L"Выделение (M): лассо; щелчок — волшебная палочка. Shift добавляет, Alt вычитает, Shift+Alt пересекает";
        case Tool::Fill: return L"Заливка (G) по цвету композита; Shift — по всему холсту";
        case Tool::Transform: return L"Перемещение (Ctrl+T) слоя или выделенного; стрелки сдвигают на 1 px";
        case Tool::Crop: return L"Кадрирование: рамка, затем Enter; Esc отменяет";
        default: return std::wstring(toolName(tool)) + L" — появится в следующих фазах";
        }
    }
    case Hit::Profile:
        return std::format(L"Кисть: {} — каталог кистей (F6)", widen(catalog[std::min(state.brush, catalog.size() - 1)].name));
    case Hit::Color: return L"Цвет кисти — круг HSV";
    case Hit::Size:
    case Hit::SizeValue: return L"Размер кисти: перетащите, введите число или [ и ]; стрелки ±0.25 px, Shift ×10";
    case Hit::Opacity:
    case Hit::OpacityValue: return L"Непрозрачность кисти; стрелки ±1 %, Shift ×10";
    case Hit::Stabilizer:
    case Hit::StabilizerValue:
        return toleranceMode(state.tool) ? L"Допуск заливки и палочки: наибольшая разница каналов PMA" : L"Стабилизация штриха, мс";
    case Hit::More: return L"Дополнительные параметры";
    case Hit::Splitter: return L"Ширина дока";
    case Hit::DockAction: {
        constexpr std::wstring_view kHelp[] = {L"Новый слой (Ctrl+Shift+N)", L"Дублировать слой", L"Удалить слой",
                                               L"Новая группа", L"Менеджер слоёв (Ctrl+F7) появится позже"};
        return std::wstring(kHelp[std::clamp(h.index, 0, 4)]);
    }
    case Hit::LayerVisibility: return L"Показать или скрыть слой";
    case Hit::LayerRow: return L"Щелчок выбирает слой, перетаскивание переставляет, двойной щелчок — имя; Alt+↑/↓";
    case Hit::LayerLock: return L"Запретить изменение пикселей слоя";
    case Hit::LayerClip: return L"Обрезать по нижнему слою";
    case Hit::BlendButton: return L"Режим наложения слоя";
    case Hit::BlendItem: return std::wstring(L"Режим ") + widen(core::blendModeName(h.index));
    case Hit::AlphaLockBox: return L"Alpha Lock: рисовать только по уже закрашенному";
    case Hit::FxCurve: return L"Щелчок добавляет точку, перетаскивание двигает, двойной щелчок удаляет";
    case Hit::FxSlider: return L"Перетащите или выберите стрелками ↑↓ и меняйте ←→ (Shift ×10)";
    case Hit::ClipBox: return L"Обрезать по нижнему слою";
    case Hit::LayerOpacity:
    case Hit::LayerOpacityValue: return L"Непрозрачность слоя";
    case Hit::HueRing: return L"Оттенок";
    case Hit::SvSquare: return L"Насыщенность и яркость";
    case Hit::ColorPrevious: return L"Вернуть прежний цвет";
    case Hit::CatalogItem:
    case Hit::DockBrush: return L"Щелчок или Enter выбирает кисть";
    default: return {};
    }
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void EditorUi::focusRing(UiRenderer& r, const Rect& box) { r.stroke(inflate(box, 2), theme_.focus, 2, 5); }

void EditorUi::drawField(UiRenderer& r, const Rect& box, Field field, const std::wstring& text, bool focused) {
    const bool editing = editField_ == field && field != Field::None;
    r.fill(box, editing ? theme_.window : theme_.control, 3);
    if (editing) r.stroke(box, theme_.accent, 1, 3);
    const Rect inner{box.x + 6, box.y, box.w - 12, box.h};
    const std::wstring shown = editing ? editText_ : text;
    r.textIn(shown, inner, kText, theme_.text, editing ? Align::Left : Align::Right);
    if (editing) {
        const double w = std::min(r.textWidth(shown, kText), inner.w);
        if (editFresh_) {
            r.fill({inner.x - 2, box.y + 5, w + 4, box.h - 10}, rgb8(0x4C9BFF, 0.35f), 2);  // selected: typing replaces it
        } else {
            r.fill({inner.x + w + 1, box.y + 5, 1.5, box.h - 10}, theme_.text);
        }
    }
    if (focused && !editing) focusRing(r, box);
}

void EditorUi::drawSlider(UiRenderer& r, const Rect& control, std::wstring_view label, double t, Field field,
                          const std::wstring& valueText, bool hot, bool focused) {
    if (control.empty()) return;
    const SliderGeometry g = sliderGeometry(control);
    r.text(label, g.x0, control.y + 1, kSmall, theme_.textDim);
    const double barY = control.y + 21;
    r.fill({g.x0, barY, g.x1 - g.x0, 4}, theme_.control, 2);
    const double thumb = g.x0 + t * (g.x1 - g.x0);
    r.fill({g.x0, barY, thumb - g.x0, 4}, theme_.accent, 2);
    r.disc(thumb, barY + 2, hot ? 7 : 6, theme_.accent);
    r.ring(thumb, barY + 2, hot ? 7 : 6, 1.5, theme_.window);
    drawField(r, g.value, field, valueText, false);
    if (focused && editField_ != field) focusRing(r, control);
}

void EditorUi::draw(UiRenderer& r, const UiState& state) {
    // Keep the dock picker in step with colour changes from elsewhere (pipette, HEX).
    if (drag_ != Drag::Hue && drag_ != Drag::Sv) {
        const core::Rgb shown = core::hsvToRgb(hsv_);
        if (std::abs(shown[0] - state.color[0]) + std::abs(shown[1] - state.color[1]) +
                std::abs(shown[2] - state.color[2]) > 1.0 / 256) {
            syncHsv(state);
        }
    }
    const auto& catalog = catalog_;
    const core::BrushProfile& brush = catalog[std::min(state.brush, catalog.size() - 1)];

    // Main menu titles (the dropdown is an overlay).
    r.fill(layout_.menu, theme_.menuBar);
    double x = 8;
    for (std::size_t i = 0; i < menus_.size(); ++i) {
        Menu& m = menus_[i];
        const double w = r.textWidth(m.title, kText) + 20;
        m.titleRect = {x, 0, w, 32};
        const bool open = menuOpen_ == static_cast<int>(i);
        const bool hot = hover_.kind == Hit::MenuTitle && hover_.index == static_cast<int>(i);
        if (open || hot) r.fill(m.titleRect.inset(3), open ? theme_.controlPressed : theme_.controlHover, 4);
        r.textIn(m.title, m.titleRect, kText, theme_.text, Align::Center);
        x += w;
    }

    // Tool options.
    if (layout_.optionsVisible) {
        r.fill(layout_.options, theme_.options);
        const bool profileHot = hover_.kind == Hit::Profile || popup_ == Popup::Catalog;
        r.fill(layout_.profile, profileHot ? theme_.controlHover : theme_.control, 4);
        r.textIn(widen(brush.name), {layout_.profile.x + 10, layout_.profile.y, layout_.profile.w - 34, layout_.profile.h},
                 kText, theme_.text, Align::Left);
        drawIcon(r, Icon::ChevronDown, {layout_.profile.right() - 24, layout_.profile.y + 8, 16, 16}, theme_.textDim);
        if (focus_ == Focus::Profile) focusRing(r, layout_.profile);

        const Rect swatch = layout_.color.inset(4);
        r.fill(layout_.color, hover_.kind == Hit::Color ? theme_.controlHover : theme_.control, 4);
        r.checker(swatch, 4);
        r.fill(swatch, {static_cast<float>(state.color[0]), static_cast<float>(state.color[1]),
                        static_cast<float>(state.color[2]), static_cast<float>(state.alpha)});
        r.stroke(swatch, theme_.border, 1);
        if (focus_ == Focus::Color) focusRing(r, layout_.color);

        drawSlider(r, layout_.size, L"Размер", tFromDiameter(state.diameter), Field::Size,
                   std::format(L"{:.1f} px", state.diameter), drag_ == Drag::Size, focus_ == Focus::Size);
        drawSlider(r, layout_.opacity, L"Непрозрачность", state.opacity, Field::Opacity,
                   std::format(L"{:.0f} %", state.opacity * 100), drag_ == Drag::Opacity, focus_ == Focus::Opacity);
        if (toleranceMode(state.tool)) {
            drawSlider(r, layout_.stabilizer, L"Допуск", state.tolerance, Field::Stabilizer,
                       std::format(L"{:.0f} %", state.tolerance * 100), drag_ == Drag::Stabilizer && popup_ != Popup::More,
                       focus_ == Focus::Stabilizer && popup_ != Popup::More);
        } else {
            drawSlider(r, layout_.stabilizer, L"Стабилизация", state.stabilizerMs / 200.0, Field::Stabilizer,
                       std::format(L"{:.0f} мс", state.stabilizerMs), drag_ == Drag::Stabilizer && popup_ != Popup::More,
                       focus_ == Focus::Stabilizer && popup_ != Popup::More);
        }
        if (!layout_.more.empty()) {
            r.fill(layout_.more, hover_.kind == Hit::More || popup_ == Popup::More ? theme_.controlHover : theme_.control, 4);
            drawIcon(r, Icon::More, layout_.more.inset(6), theme_.text);
            if (focus_ == Focus::More) focusRing(r, layout_.more);
        }
    }

    // Toolbar.
    if (layout_.toolbarVisible) {
        r.fill(layout_.toolbar, theme_.toolbar);
        r.pushClip(layout_.toolbar);
        for (std::size_t i = 0; i < layout_.tools.size(); ++i) {
            const Rect& b = layout_.tools[i];
            const auto tool = static_cast<Tool>(i);
            const bool active = state.tool == tool;
            const bool available = toolAvailable(tool);
            const bool hot = hover_.kind == Hit::Tool && hover_.index == static_cast<int>(i) && available;
            if (active) {
                r.fill(b, theme_.accent, 6);
            } else if (hot) {
                r.fill(b, theme_.controlHover, 6);
            }
            const Color ink = active ? theme_.accentText : available ? theme_.text : theme_.textDisabled;
            drawIcon(r, static_cast<Icon>(i), b.inset(8), ink);
        }
        r.popClip();
    }

    if (layout_.dockVisible) {
        r.fill(layout_.splitter, drag_ == Drag::Splitter || hover_.kind == Hit::Splitter ? theme_.accent : theme_.splitter);
        drawDock(r, state);
    }

    // Status bar: what the pointer is over, or the tool; document facts on the right.
    r.fill(layout_.status, theme_.statusBar);
    std::wstring left = state.notice.empty() ? helpFor(hover_, state) : state.notice;
    if (left.empty()) left = std::format(L"{} · {}", toolName(state.tool), widen(brush.name));
    const double rightWidth = std::min(r.textWidth(state.status, kStatus), layout_.status.w * 0.6);
    r.textIn(state.status, {layout_.status.right() - rightWidth - 10, layout_.status.y, rightWidth, layout_.status.h},
             kStatus, theme_.textDim, Align::Right);
    r.textIn(left, {layout_.status.x + 10, layout_.status.y, layout_.status.w - rightWidth - 30, layout_.status.h}, kStatus,
             theme_.textDim, Align::Left);
}

void EditorUi::drawDock(UiRenderer& r, const UiState& state) {
    const Rect& d = layout_.dock;
    r.fill(d, theme_.dock);
    r.pushClip(d);
    // Tabs: «Слои», «Цвет», «Кисти».
    constexpr std::wstring_view kTabs[] = {L"Слои", L"Цвет", L"Кисти"};
    const double tabW = layout_.dockTabs.w / 3;
    for (int i = 0; i < 3; ++i) {
        const Rect tab{layout_.dockTabs.x + tabW * i, layout_.dockTabs.y, tabW, 36};
        const bool active = dockTab_ == i;
        r.fill(tab, active ? theme_.dock : theme_.options);
        if (active) r.fill({tab.x + 8, tab.bottom() - 3, tab.w - 16, 2}, theme_.accent, 1);
        r.textIn(kTabs[i], tab, kText, active ? theme_.text : theme_.textDim, Align::Center, active);
    }

    if (dockTab_ == 0) {
        drawLayers(r, state);
    } else if (dockTab_ == 1) {
        drawPicker(r, picker(dockPickerFrame()), state, true);
    } else {
        const auto& catalog = catalog_;
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            const Rect row{d.x + 4, d.y + 40 + kItemRow * static_cast<double>(i), d.w - 8, kItemRow - 2};
            const bool selected = state.brush == i;
            const bool hot = hover_.kind == Hit::DockBrush && hover_.index == static_cast<int>(i);
            if (selected || hot) r.fill(row, selected ? theme_.selection : theme_.controlHover, 4);
            r.textIn(widen(catalog[i].name), {row.x + 10, row.y, row.w - 100, row.h}, kText, theme_.text, Align::Left,
                     selected);
            r.textIn(std::format(L"Ø {:.0f} px", catalog[i].tip.diameter),
                     {row.right() - 90, row.y, 80, row.h}, kSmall, theme_.textDim, Align::Right);
        }
    }
    r.popClip();
}

void EditorUi::drawLayers(UiRenderer& r, const UiState& state) {
    const core::LayerNode* active = activeLayer(state);
    // Actions: add, duplicate, delete, group, manager.
    constexpr Icon kActions[] = {Icon::Plus, Icon::Duplicate, Icon::Trash, Icon::Folder, Icon::Layers};
    for (int i = 0; i < 5; ++i) {
        const Rect b{layout_.dockActions.x + 8 + 32.0 * i, layout_.dockActions.y + 4, 28, 28};
        const bool enabled = i == 0 || i == 3 || (i < 3 && active);
        if (enabled && hover_.kind == Hit::DockAction && hover_.index == i) r.fill(b, theme_.controlHover, 4);
        drawIcon(r, kActions[i], b.inset(2), enabled ? theme_.text : theme_.textDisabled);
    }

    // The list, top of the stack first.
    const std::vector<LayerRowInfo> rows = layerRows(state);
    const Rect& list = layout_.dockList;
    r.pushClip(list);
    const bool reordering = drag_ == Drag::LayerRow && dragMoved_;
    const DropTarget target = reordering ? dropTarget(dragY_, state) : DropTarget{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Rect rr = rowRect(i);
        if (rr.bottom() < list.y || rr.y > list.bottom()) continue;
        const core::LayerNode& n = *state.layers->find(rows[i].id);
        const bool isActive = active && active->id == n.id;
        const bool hot = (hover_.kind == Hit::LayerRow || hover_.kind == Hit::LayerVisibility || hover_.kind == Hit::LayerLock ||
                          hover_.kind == Hit::LayerClip) &&
                         hover_.index == static_cast<int>(i);
        if (isActive) {
            r.fill(rr, theme_.selection, 4);
        } else if (hot) {
            r.fill(rr, theme_.controlHover, 4);
        }
        if (reordering && target.valid && target.into == n.id) r.stroke(rr, theme_.accent, 2, 4);
        if (reordering && dragLayer_ == n.id) r.fill(rr, rgb8(0x000000, 0.25f), 4);
        const bool shown = state.layers->effectivelyVisible(n.id);
        drawIcon(r, n.props.visible ? Icon::Eye : Icon::EyeOff, {rr.x + 6, rr.y + 9, 20, 20},
                 hover_.kind == Hit::LayerVisibility && hover_.index == static_cast<int>(i) ? theme_.text : theme_.textDim);
        const double indent = rr.x + 32 + 14.0 * rows[i].depth;
        if (n.kind == core::NodeKind::Group) {
            const bool open = !collapsed_.contains(n.id);
            const Rect chevron{indent, rr.y + 11, 16, 16};
            if (open) {
                drawIcon(r, Icon::ChevronDown, chevron, theme_.textDim);
            } else {
                r.line(chevron.x + 5, chevron.y + 4, chevron.x + 10, chevron.y + 8, 1.5, theme_.textDim);
                r.line(chevron.x + 10, chevron.y + 8, chevron.x + 5, chevron.y + 12, 1.5, theme_.textDim);
            }
        }
        const Rect thumb{indent + 18, rr.y + 3, 32, 32};
        if (n.kind != core::NodeKind::Group) {
            r.fill(thumb, {1, 1, 1, 1});
            host_.drawLayerThumbnail(r, n.id, thumb);
            r.stroke(thumb, n.kind == core::NodeKind::Vector ? theme_.accent : theme_.border, 1);
        } else {
            drawIcon(r, Icon::Folder, thumb.inset(4), theme_.textDim);
        }
        const Rect nameBox{thumb.right() + 8, rr.y, std::max(0.0, rr.right() - 48 - thumb.right() - 12), rr.h};
        if (editField_ == Field::LayerName && renameId_ == n.id) {
            drawField(r, {nameBox.x - 4, rr.y + 6, nameBox.w + 8, rr.h - 12}, Field::LayerName, editText_, false);
        } else {
            std::wstring label = widen(n.props.name);
            if (n.props.blendId != 0) label += std::format(L" · {}", widen(core::blendModeName(n.props.blendId)));
            if (n.props.opacity < 1.0) label += std::format(L" · {:.0f}%", n.props.opacity * 100);
            r.textIn(label, nameBox, kText, shown ? theme_.text : theme_.textDisabled, Align::Left, isActive);
        }
        const bool lockHot = hover_.kind == Hit::LayerLock && hover_.index == static_cast<int>(i);
        if (n.props.locked || lockHot) {
            drawIcon(r, Icon::Lock, {rr.right() - 45, rr.y + 10, 18, 18}, n.props.locked ? theme_.text : theme_.textDisabled);
        }
        const bool clipHot = hover_.kind == Hit::LayerClip && hover_.index == static_cast<int>(i);
        if (n.props.clipToBase || clipHot) {
            drawIcon(r, Icon::Clip, {rr.right() - 22, rr.y + 10, 18, 18},
                     n.props.clipToBase ? (state.layers->clipBase(n.id) ? theme_.accent : theme_.danger) : theme_.textDisabled);
        }
        if (focus_ == Focus::LayerList && isActive && editField_ != Field::LayerName) focusRing(r, rr);
    }
    if (reordering && target.valid && target.into == 0) {
        r.fill({target.barX, target.barY - 1, list.right() - 8 - target.barX, 2}, theme_.accent, 1);
        r.disc(target.barX, target.barY, 3, theme_.accent);
    }
    r.popClip();

    // Properties of the active layer.
    const Rect& p = layout_.dockProperties;
    r.fill({p.x, p.y, p.w, 1}, theme_.border);
    if (!active) return;
    const PropertyRects pr = propertyRects();
    drawSlider(r, pr.opacity, L"Непрозрачность слоя", active->props.opacity, Field::LayerOpacity,
               std::format(L"{:.0f} %", active->props.opacity * 100), drag_ == Drag::LayerOpacity, focus_ == Focus::LayerOpacity);
    const bool blendHot = hover_.kind == Hit::BlendButton || popup_ == Popup::Blend;
    r.fill(pr.blend, blendHot ? theme_.controlHover : theme_.control, 4);
    r.textIn(widen(core::blendModeName(active->props.blendId)), {pr.blend.x + 10, pr.blend.y, pr.blend.w - 30, pr.blend.h},
             kText, theme_.text, Align::Left);
    drawIcon(r, Icon::ChevronDown, {pr.blend.right() - 22, pr.blend.y + 8, 16, 16}, theme_.textDim);
    if (focus_ == Focus::Blend) focusRing(r, pr.blend);
    const auto checkbox = [&](const Rect& area, std::wstring_view label, bool on, bool enabled, bool hot) {
        const Rect box{area.x, area.y + 8, 16, 16};
        const Color ink = enabled ? theme_.text : theme_.textDisabled;
        if (on && enabled) {
            r.fill(box, theme_.accent, 3);
            drawIcon(r, Icon::Check, box.inset(1), theme_.accentText);
        } else {
            r.stroke(box, hot && enabled ? theme_.text : (enabled ? theme_.textDim : theme_.textDisabled), 1.5, 3);
        }
        r.textIn(label, {area.x + 22, area.y, area.w - 22, area.h}, kSmall, ink, Align::Left);
    };
    checkbox(pr.alphaLock, L"Alpha Lock", active->props.alphaLock, active->kind == core::NodeKind::Raster,
             hover_.kind == Hit::AlphaLockBox);
    checkbox(pr.clip, L"Обрезка", active->props.clipToBase, true, hover_.kind == Hit::ClipBox);
}

// ── Filter dialog (§9) ───────────────────────────────────────────────────────

void EditorUi::openFx(core::FxKind kind) {
    menuOpen_ = -1;
    if (editField_ != Field::None) cancelEdit();
    fx_ = core::defaultFx(kind);
    fxChannel_ = 0;
    fxFocus_ = 0;
    popup_ = Popup::Fx;
    host_.fxChanged(fx_);  // §9: the preview takes its snapshot when the dialog opens
}

void EditorUi::closeFx(bool apply) {
    popup_ = Popup::None;
    drag_ = drag_ == Drag::FxSlider || drag_ == Drag::FxCurve ? Drag::Swallow : drag_;
    if (apply) {
        host_.fxApply(fx_);
    } else {
        host_.fxCancel();
    }
}

std::vector<EditorUi::FxParamInfo> EditorUi::fxParams() const {
    using K = core::FxKind;
    const FxParamInfo amount{FxParam::Amount, L"Сила", 0.0, 1.0, false};
    switch (fx_.kind) {
    case K::Gaussian: return {{FxParam::Radius, L"Радиус, px", 0, 32, true}, amount};
    case K::ToneCurve: return {amount};
    case K::Unsharp:
        return {{FxParam::Radius, L"Радиус, px", 0, 32, true}, {FxParam::Gain, L"Усиление", 0, 4, false},
                {FxParam::Threshold, L"Порог", 0, 1, false}, amount};
    case K::Chromatic: return {{FxParam::ShiftX, L"Сдвиг X, px", -32, 32, false}, {FxParam::ShiftY, L"Сдвиг Y, px", -32, 32, false}, amount};
    case K::AnimeLines:
        return {{FxParam::Radius, L"Размытие, px", 0, 8, true}, {FxParam::Black, L"Чёрная точка", 0, 1, false},
                {FxParam::White, L"Белая точка", 0, 1, false},     {FxParam::Mid, L"Середина", 0.01, 0.99, false},
                {FxParam::Edge, L"Контуры", 0, 4, false},           amount};
    }
    return {amount};
}

double EditorUi::fxValue(FxParam id) const {
    switch (id) {
    case FxParam::Radius: return fx_.radius;
    case FxParam::Gain: return fx_.gain;
    case FxParam::Threshold: return fx_.threshold;
    case FxParam::ShiftX: return fx_.shiftX;
    case FxParam::ShiftY: return fx_.shiftY;
    case FxParam::Black: return fx_.blackPoint;
    case FxParam::White: return fx_.whitePoint;
    case FxParam::Mid: return fx_.mid;
    case FxParam::Edge: return fx_.edgeGain;
    case FxParam::Amount: return fx_.amount;
    }
    return 0.0;
}

void EditorUi::setFxValue(FxParam id, double v) {
    switch (id) {
    case FxParam::Radius: fx_.radius = static_cast<int>(std::lround(v)); break;
    case FxParam::Gain: fx_.gain = v; break;
    case FxParam::Threshold: fx_.threshold = v; break;
    case FxParam::ShiftX: fx_.shiftX = v; break;
    case FxParam::ShiftY: fx_.shiftY = v; break;
    case FxParam::Black: fx_.blackPoint = v; break;
    case FxParam::White: fx_.whitePoint = v; break;
    case FxParam::Mid: fx_.mid = v; break;
    case FxParam::Edge: fx_.edgeGain = v; break;
    case FxParam::Amount: fx_.amount = v; break;
    }
    fx_ = core::sanitize(fx_);
}

double EditorUi::fxDialogHeight() const {
    double h = 44 + kFxRow * static_cast<double>(fxParams().size());
    if (fx_.kind == core::FxKind::ToneCurve) h += 4 + 26 + 8 + kFxCurveHeight + 8;
    if (fx_.kind == core::FxKind::ToneCurve || fx_.kind == core::FxKind::AnimeLines) h += 36;
    return h + 56;
}

Rect EditorUi::fxParamRect(std::size_t index) const {
    const Rect box = popupRect();
    return {box.x + 16, box.y + 48 + kFxRow * static_cast<double>(index), box.w - 32, 32};
}

Rect EditorUi::fxChannelRect(int channel) const {
    const Rect box = popupRect();
    const double top = box.y + 44 + kFxRow * static_cast<double>(fxParams().size()) + 4;
    const double w = (box.w - 32) / 4;
    return {box.x + 16 + w * channel, top, w - 4, 26};
}

Rect EditorUi::fxCurveRect() const {
    const Rect tabs = fxChannelRect(0);
    const Rect box = popupRect();
    return {box.x + 16, tabs.bottom() + 8, box.w - 32, kFxCurveHeight};
}

Rect EditorUi::fxCheckRect() const {
    if (fx_.kind != core::FxKind::ToneCurve && fx_.kind != core::FxKind::AnimeLines) return {};
    const Rect box = popupRect();
    const double top = fx_.kind == core::FxKind::ToneCurve ? fxCurveRect().bottom() + 8
                                                           : box.y + 44 + kFxRow * static_cast<double>(fxParams().size());
    return {box.x + 16, top, box.w - 32, 28};
}

Rect EditorUi::fxApplyRect() const {
    const Rect box = popupRect();
    return {box.right() - 16 - 120, box.bottom() - 44, 120, 32};
}

Rect EditorUi::fxCancelRect() const {
    const Rect apply = fxApplyRect();
    return {apply.x - 8 - 100, apply.y, 100, 32};
}

void EditorUi::fxCurvePress(double x, double y) {
    const Rect c = fxCurveRect();
    auto& points = fx_.curve.channels[static_cast<std::size_t>(fxChannel_)];
    const double u = std::clamp((x - c.x) / c.w, 0.0, 1.0);
    const double v = std::clamp(1.0 - (y - c.y) / c.h, 0.0, 1.0);
    // A point under the pointer: drag it, or remove it on a double click (the ends stay).
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double px = c.x + points[i].x * c.w;
        const double py = c.y + (1.0 - points[i].y) * c.h;
        if (std::hypot(px - x, py - y) > 8.0) continue;
        const bool twice = pressTime_ - fxLastCurveClick_ <= GetDoubleClickTime() / 1000.0;
        fxLastCurveClick_ = pressTime_;
        if (twice && i > 0 && i + 1 < points.size()) {
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
            fxPoint_ = -1;
            host_.fxChanged(fx_);
            return;
        }
        fxPoint_ = static_cast<int>(i);
        return;
    }
    fxLastCurveClick_ = pressTime_;
    // §9.3: a new point between the ends; an existing x edits that point instead.
    if (points.size() >= 32 || u <= 0.0 || u >= 1.0) return;
    const auto at = std::lower_bound(points.begin(), points.end(), u, [](const core::CurvePoint& p, double x0) { return p.x < x0; });
    if (at != points.end() && at->x == u) {
        at->y = v;
        fxPoint_ = static_cast<int>(at - points.begin());
    } else {
        fxPoint_ = static_cast<int>(points.insert(at, core::CurvePoint{u, v}) - points.begin());
    }
    host_.fxChanged(fx_);
}

void EditorUi::fxCurveDrag(double x, double y) {
    auto& points = fx_.curve.channels[static_cast<std::size_t>(fxChannel_)];
    if (fxPoint_ < 0 || static_cast<std::size_t>(fxPoint_) >= points.size()) return;
    const Rect c = fxCurveRect();
    const auto i = static_cast<std::size_t>(fxPoint_);
    core::CurvePoint& p = points[i];
    p.y = std::clamp(1.0 - (y - c.y) / c.h, 0.0, 1.0);
    if (i > 0 && i + 1 < points.size()) {  // the ends keep x = 0 and x = 1
        const double gap = 1.0 / 255.0;
        p.x = std::clamp((x - c.x) / c.w, points[i - 1].x + gap, points[i + 1].x - gap);
    }
    host_.fxChanged(fx_);
}

void EditorUi::drawFxDialog(UiRenderer& r) {
    const Rect box = popupRect();
    r.textIn(widen(core::fxName(fx_.kind)), {box.x + 16, box.y + 4, box.w - 32, 36}, kText, theme_.text, Align::Left, true);
    const auto params = fxParams();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const FxParamInfo& p = params[i];
        const double v = fxValue(p.id);
        const double t = p.max > p.min ? (v - p.min) / (p.max - p.min) : 0.0;
        const std::wstring text = p.id == FxParam::Amount ? std::format(L"{:.0f} %", v * 100)
                                  : p.integer            ? std::format(L"{:.0f}", v)
                                                         : std::format(L"{:.2f}", v);
        drawSlider(r, fxParamRect(i), p.label, t, Field::None, text, fxSlider_ == static_cast<int>(i),
                   fxFocus_ == static_cast<int>(i));
    }
    if (fx_.kind == core::FxKind::ToneCurve) {
        constexpr std::wstring_view kChannels[] = {L"Общая", L"R", L"G", L"B"};
        constexpr Color kInk[] = {{0.9f, 0.9f, 0.92f, 1}, {0.95f, 0.35f, 0.35f, 1}, {0.35f, 0.85f, 0.4f, 1}, {0.4f, 0.55f, 1.0f, 1}};
        for (int c = 0; c < 4; ++c) {
            const Rect tab = fxChannelRect(c);
            r.fill(tab, fxChannel_ == c ? theme_.selection : theme_.control, 4);
            r.textIn(kChannels[c], tab, kSmall, theme_.text, Align::Center, fxChannel_ == c);
        }
        const Rect c = fxCurveRect();
        r.fill(c, theme_.window, 4);
        for (int k = 1; k < 4; ++k) {
            r.fill({c.x + c.w * k / 4, c.y, 1, c.h}, theme_.border);
            r.fill({c.x, c.y + c.h * k / 4, c.w, 1}, theme_.border);
        }
        r.line(c.x, c.bottom(), c.right(), c.y, 1.0, theme_.textDisabled);
        const auto& points = fx_.curve.channels[static_cast<std::size_t>(fxChannel_)];
        const Color ink = kInk[fxChannel_];
        constexpr int kSegments = 64;
        for (int k = 0; k < kSegments; ++k) {
            const double x0 = static_cast<double>(k) / kSegments, x1 = static_cast<double>(k + 1) / kSegments;
            r.line(c.x + x0 * c.w, c.y + (1 - core::evalCurve(points, x0)) * c.h, c.x + x1 * c.w,
                   c.y + (1 - core::evalCurve(points, x1)) * c.h, 2.0, ink);
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double px = c.x + points[i].x * c.w, py = c.y + (1 - points[i].y) * c.h;
            r.disc(px, py, fxPoint_ == static_cast<int>(i) ? 6 : 5, ink);
            r.ring(px, py, 5, 1.5, theme_.window);
        }
        r.stroke(c, theme_.border, 1, 4);
    }
    if (const Rect check = fxCheckRect(); !check.empty()) {
        const bool on = fx_.kind == core::FxKind::ToneCurve ? fx_.curve.discrete : fx_.toNewLayer;
        const Rect tick{check.x, check.y + 6, 16, 16};
        if (on) {
            r.fill(tick, theme_.accent, 3);
            drawIcon(r, Icon::Check, tick.inset(1), theme_.accentText);
        } else {
            r.stroke(tick, theme_.textDim, 1.5, 3);
        }
        r.textIn(fx_.kind == core::FxKind::ToneCurve ? L"Дискретная таблица" : L"На новый слой",
                 {check.x + 24, check.y, check.w - 24, check.h}, kText, theme_.text, Align::Left);
    }
    const Rect apply = fxApplyRect();
    const Rect cancel = fxCancelRect();
    r.fill(apply, theme_.accent, 4);
    r.textIn(L"Применить", apply, kText, theme_.accentText, Align::Center, true);
    r.fill(cancel, hover_.kind == Hit::FxCancel ? theme_.controlHover : theme_.control, 4);
    r.textIn(L"Отмена", cancel, kText, theme_.text, Align::Center);
}

void EditorUi::drawBlendPopup(UiRenderer& r, const UiState& state) {
    const core::LayerNode* active = activeLayer(state);
    for (int m = 0; m < core::kBlendModeCount; ++m) {
        const Rect item = blendItemRect(m);
        const bool current = active && active->props.blendId == m;
        const bool hot = (hover_.kind == Hit::BlendItem && hover_.index == m) || blendHighlight_ == m;
        if (hot) r.fill(item, theme_.selection, 4);
        if (current) r.fill({item.x, item.y + 5, 3, item.h - 10}, theme_.accent, 1.5);
        r.textIn(widen(core::blendModeName(m)), {item.x + 10, item.y, item.w - 12, item.h}, kSmall, theme_.text,
                 Align::Left, current);
    }
}

void EditorUi::drawPicker(UiRenderer& r, const PickerGeometry& g, const UiState& state, bool inDock) {
    r.hueRing(g.cx, g.cy, kRingInner, kRingOuter);
    const double angle = hsv_.h * 2 * std::numbers::pi;
    const double mid = (kRingInner + kRingOuter) / 2;
    const double hx = g.cx + mid * std::cos(angle);
    const double hy = g.cy - mid * std::sin(angle);
    r.ring(hx, hy, 8.5, 1.5, rgb8(0x000000, 0.7f));
    r.ring(hx, hy, 7, 2, rgb8(0xFFFFFF));

    r.svSquare(g.square, hsv_.h);
    const double sx = g.square.x + hsv_.s * g.square.w;
    const double sy = g.square.y + (1 - hsv_.v) * g.square.h;
    r.ring(sx, sy, 6.5, 1.5, rgb8(0x000000, 0.7f));
    r.ring(sx, sy, 5, 2, rgb8(0xFFFFFF));

    constexpr Field kFields[] = {Field::H, Field::S, Field::V, Field::A, Field::Hex};
    constexpr Focus kFocus[] = {Focus::H, Focus::S, Focus::V, Focus::A, Focus::Hex};
    constexpr std::wstring_view kPrefix[] = {L"H ", L"S ", L"V ", L"A ", L""};
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        const bool editingHere = editField_ == kFields[i] && (inDock == (popup_ != Popup::Color));
        const std::wstring value = fieldText(kFields[i], state);
        if (editingHere) {
            drawField(r, g.fields[i], kFields[i], value, false);
        } else {
            r.fill(g.fields[i], theme_.control, 3);
            r.textIn(std::wstring(kPrefix[i]) + value, {g.fields[i].x + 6, g.fields[i].y, g.fields[i].w - 12, g.fields[i].h},
                     kSmall, theme_.text, Align::Left);
            if (!inDock && focus_ == kFocus[i]) focusRing(r, g.fields[i]);
        }
    }
    // Current and previous colour (§11.1: shown until confirmed).
    const auto swatch = [&](const Rect& box, const core::Rgb& c, double a) {
        r.checker(box, 4);
        r.fill(box, {static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]), static_cast<float>(a)});
    };
    swatch(g.current, state.color, state.alpha);
    if (!inDock) {
        swatch(g.previous, colorAtOpen_, alphaAtOpen_);
        r.stroke({g.current.x, g.current.y, g.current.w + g.previous.w, g.current.h}, theme_.border, 1, 2);
    } else {
        r.stroke(g.current, theme_.border, 1, 2);
    }
}

void EditorUi::drawCatalog(UiRenderer& r, const UiState& state) {
    const Rect box = popupRect();
    const CatalogLayout c = catalogLayout(box);
    const auto& catalog = catalog_;

    // Search field.
    const bool searching = focus_ == Focus::Search;
    r.fill(c.search, searching ? theme_.window : theme_.control, 4);
    if (searching) r.stroke(c.search, theme_.accent, 1, 4);
    drawIcon(r, Icon::Search, {c.search.x + 6, c.search.y + 8, 16, 16}, theme_.textDim);
    const Rect textBox{c.search.x + 28, c.search.y, c.search.w - 36, c.search.h};
    if (search_.empty()) {
        r.textIn(L"Поиск кистей", textBox, kText, theme_.textDisabled, Align::Left);
    } else {
        r.textIn(search_, textBox, kText, theme_.text, Align::Left);
    }
    if (searching) {
        const double w = std::min(r.textWidth(search_, kText), textBox.w);
        r.fill({textBox.x + w + 1, c.search.y + 8, 1.5, c.search.h - 16}, theme_.text);
    }

    // Preview of the highlighted (or current) brush with the real engine.
    const std::size_t shown = std::min(highlight_.value_or(state.brush), catalog.size() - 1);
    r.fill(c.preview, {1, 1, 1, 1}, 4);
    host_.drawBrushPreview(r, shown, c.preview.inset(2));
    r.stroke(c.preview, theme_.border, 1, 4);

    // Categories.
    for (std::size_t i = 0; i < std::size(kCategories); ++i) {
        const Rect row{c.categories.x, c.categories.y + kCategoryRow * static_cast<double>(i), c.categories.w, kCategoryRow - 2};
        const bool active = category_ == static_cast<int>(i);
        if (active) r.fill(row, theme_.selection, 4);
        r.textIn(kCategories[i], {row.x + 10, row.y, row.w - 12, row.h}, kText, active ? theme_.text : theme_.textDim, Align::Left);
        if (active && focus_ == Focus::Categories) focusRing(r, row);
    }

    // Profiles.
    const auto items = catalogItems();
    r.pushClip(c.list);
    for (std::size_t k = 0; k < items.size(); ++k) {
        const std::size_t i = items[k];
        const Rect row{c.list.x, c.list.y + kItemRow * static_cast<double>(k), c.list.w, kItemRow - 2};
        const bool selected = state.brush == i;
        const bool highlighted = highlight_ == i;
        if (highlighted) r.fill(row, theme_.controlHover, 4);
        if (selected) r.fill({row.x, row.y + 6, 3, row.h - 12}, theme_.accent, 1.5);
        r.textIn(widen(catalog[i].name), {row.x + 12, row.y, row.w - 96, row.h}, kText, theme_.text, Align::Left,
                 selected);
        r.textIn(std::format(L"Ø {:.0f} px", catalog[i].tip.diameter),
                 {row.right() - 84, row.y, 76, row.h}, kSmall, theme_.textDim, Align::Right);
        if (highlighted && focus_ == Focus::Items) focusRing(r, row);
    }
    if (items.empty()) r.textIn(L"Ничего не найдено", {c.list.x, c.list.y, c.list.w, 40}, kText, theme_.textDim, Align::Center);
    r.popClip();
}

void EditorUi::drawMenus(UiRenderer& r, const UiState& state) {
    if (menuOpen_ < 0) return;
    const Rect drop = menuDropRect(menuOpen_);
    r.fill({drop.x + 3, drop.y + 4, drop.w, drop.h}, theme_.shadow, 6);
    r.fill(drop, theme_.popup, 6);
    r.stroke(drop, theme_.border, 1, 6);
    double top = drop.y + 4;
    const auto& entries = menus_[static_cast<std::size_t>(menuOpen_)].entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const MenuEntry& e = entries[i];
        const Rect row{drop.x + 4, top, drop.w - 8, kMenuRow};
        const bool enabled = menuEnabled(e.action, state);
        if (menuItem_ == static_cast<int>(i) && enabled) r.fill(row, theme_.selection, 4);
        const Color ink = enabled ? theme_.text : theme_.textDisabled;
        if (const core::LayerNode* active = activeLayer(state);
            active && e.action >= kToggleFlag && e.action < kToggleFlag + 4) {
            const auto flag = static_cast<LayerFlag>(e.action - kToggleFlag);
            const bool on = (flag == LayerFlag::ClipToBase && active->props.clipToBase) ||
                            (flag == LayerFlag::AlphaLock && active->props.alphaLock) ||
                            (flag == LayerFlag::Locked && active->props.locked);
            if (on) drawIcon(r, Icon::Check, {row.right() - 26, row.y + 6, 16, 16}, ink);
        }
        r.textIn(e.label, {row.x + 10, row.y, row.w - 100, row.h}, kText, ink, Align::Left);
        r.textIn(e.shortcut, {row.right() - 100, row.y, 90, row.h}, kSmall, enabled ? theme_.textDim : theme_.textDisabled,
                 Align::Right);
        top += kMenuRow;
        if (e.separatorAfter) {
            r.fill({drop.x + 10, top + 4, drop.w - 20, 1}, theme_.border);
            top += kMenuSeparator;
        }
    }
}

void EditorUi::drawOverlays(UiRenderer& r, const UiState& state) {
    if (popup_ != Popup::None) {
        const Rect box = popupRect();
        r.fill({box.x + 4, box.y + 6, box.w, box.h}, theme_.shadow, 10);
        r.fill(box, theme_.popup, 8);
        r.stroke(box, theme_.border, 1, 8);
        switch (popup_) {
        case Popup::Catalog: drawCatalog(r, state); break;
        case Popup::Color: drawPicker(r, picker(box), state, false); break;
        case Popup::Blend: drawBlendPopup(r, state); break;
        case Popup::Fx: drawFxDialog(r); break;
        case Popup::More:
            if (toleranceMode(state.tool)) {
                drawSlider(r, moreSliderRect(), L"Допуск", state.tolerance, Field::Stabilizer,
                           std::format(L"{:.0f} %", state.tolerance * 100), drag_ == Drag::Stabilizer, focus_ == Focus::Stabilizer);
            } else {
                drawSlider(r, moreSliderRect(), L"Стабилизация", state.stabilizerMs / 200.0, Field::Stabilizer,
                           std::format(L"{:.0f} мс", state.stabilizerMs), drag_ == Drag::Stabilizer,
                           focus_ == Focus::Stabilizer);
            }
            break;
        case Popup::None: break;
        }
    }
    drawMenus(r, state);
}

}  // namespace chroma::ui

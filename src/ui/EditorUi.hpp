#pragma once
// ChromaStudio Engine · ui · the editor chrome (spec §11).
//
// Owns the layout state (dock shown and width, panels hidden, toolbar scroll), the controls and
// their interaction states (§11.2): hover, DragControl with capture and Esc to restore the snapshot,
// popups (brush catalogue, HSV, "more", blend modes) where the first click outside closes and is
// swallowed, the main menu (F10 / Alt), keyboard focus with Tab / Shift+Tab inside a panel, the
// layer list of the dock (§11.1: rows of 40 DIP, top of the list = top of the stack, drag and drop
// with an insertion bar after 4 DIP, nesting under groups, double click to rename) and the status bar.
// The document itself stays with the host: the chrome reports user intent through UiHost and
// reads the current values from UiState.

#include "PlatformWindow.hpp"
#include "core/Color.hpp"
#include "core/BrushProfile.hpp"
#include "core/Fx.hpp"
#include "core/LayerTree.hpp"
#include "ui/Layout.hpp"
#include "ui/Theme.hpp"
#include "ui/UiRenderer.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chroma::ui {

enum class Tool : std::uint8_t { Brush, Eraser, Smudge, Blur, Fill, Select, Vector, Transform, Crop, Pipette, Hand };

/// Tools that work in this version.
bool toolAvailable(Tool tool) noexcept;
std::wstring_view toolName(Tool tool) noexcept;

enum class Command : std::uint8_t {
    NewDocument, Open, Save, SaveAs, ExportPng, Exit, Undo, Redo, FitView, ActualPixels, About,
    NewLayer, NewGroup, DuplicateLayer, DeleteLayer, LayerUp, LayerDown,
    Cut, Copy, Paste, SelectAll, Deselect, InvertSelection,
    NewVectorLayer, RasterizeLayer, ImportBrush, PasteBrush, ImportImage,
};

enum class LayerFlag : std::uint8_t { Visible, AlphaLock, ClipToBase, Locked };

enum class CursorKind : std::uint8_t { Arrow, Cross, Move, ResizeWE, IBeam };

/// What the chrome shows; refreshed by the host for every event and frame.
struct UiState {
    Tool tool = Tool::Brush;
    std::size_t brush = 0;
    double diameter = 12.0;
    double opacity = 1.0;  // 0..1
    double stabilizerMs = 10.0;
    double tolerance = 0.1;  // fill and magic wand, [0, 1] (§10.1)
    core::Rgb color{0.0, 0.0, 0.0};  // encoded sRGB
    double alpha = 1.0;
    bool canUndo = false;
    bool canRedo = false;
    bool painting = false;  // a canvas gesture owns the pointer
    const core::LayerTree* layers = nullptr;  // with any preview applied; valid during the call
    std::wstring status;    // right side of the status bar
    std::wstring notice;    // replaces the help on the left of the status bar when set
};

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual void selectTool(Tool tool) = 0;
    virtual void selectBrush(std::size_t index) = 0;
    virtual void setDiameter(double px) = 0;
    virtual void setOpacity(double unit) = 0;
    virtual void setStabilizer(double ms) = 0;
    virtual void setTolerance(double unit) = 0;
    virtual void setColor(const core::Rgb& srgb, double alpha) = 0;
    /// The active layer is not a document edit (no history command of its own).
    virtual void selectLayer(std::uint64_t id) = 0;
    virtual void setLayerFlag(std::uint64_t id, LayerFlag flag, bool on) = 0;
    /// `final` false while dragging (a preview), true once to commit one history command; a
    /// cancelled drag ends with final = false at the original value.
    virtual void setLayerOpacity(std::uint64_t id, double opacity, bool final) = 0;
    virtual void setLayerBlend(std::uint64_t id, int mode) = 0;
    virtual void renameLayer(std::uint64_t id, const std::string& utf8) = 0;
    /// Drag and drop: under `parent` (0 = root) at sibling `index` (0 = bottom), counted without the node.
    virtual void moveLayer(std::uint64_t id, std::uint64_t parent, std::size_t index) = 0;
    /// The filter dialog (§9): every change refreshes the preview; apply publishes one command.
    virtual void fxChanged(const core::FxSettings& settings) = 0;
    virtual void fxApply(const core::FxSettings& settings) = 0;
    virtual void fxCancel() = 0;
    virtual void run(Command command) = 0;
    /// Draws a stroke of brush `index` into `area` with the real engine; must call r.restoreState().
    virtual void drawBrushPreview(UiRenderer& r, std::size_t index, const Rect& area) = 0;
    /// The layer's own pixels over white (groups are drawn by the chrome).
    virtual void drawLayerThumbnail(UiRenderer& r, std::uint64_t id, const Rect& area) = 0;
};

class EditorUi {
public:
    explicit EditorUi(UiHost& host);

    void resize(double widthDip, double heightDip);
    /// The brushes the catalogue lists: the seven built-in ones, then the artist's library (§14).
    void setCatalog(std::vector<core::BrushProfile> brushes);
    [[nodiscard]] const std::vector<core::BrushProfile>& catalog() const noexcept { return catalog_; }
    void setTheme(const Theme& theme) { theme_ = theme; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] const ShellLayout& layout() const noexcept { return layout_; }
    /// A popup or the menu is open: the canvas takes no input.
    [[nodiscard]] bool overlayOpen() const noexcept;

    // Input; each returns true when the chrome consumed the event.
    bool pointer(const platform::PointerSample& s, const UiState& state);
    bool key(const platform::KeyEvent& k, const UiState& state);
    bool character(char32_t c, const UiState& state);
    bool wheel(const platform::WheelEvent& w, const UiState& state);
    /// Activation or capture lost: drags are abandoned and restored, overlays close.
    void abandon(const UiState& state);

    [[nodiscard]] CursorKind cursorAt(double x, double y) const;

    void draw(UiRenderer& r, const UiState& state);          // panels around the canvas
    void drawOverlays(UiRenderer& r, const UiState& state);  // popups and menus over the canvas

private:
    enum class Popup : std::uint8_t { None, Catalog, Color, More, Blend, Fx };
    enum class Hit : std::uint8_t {
        None, Canvas, Chrome,
        MenuTitle, MenuItem,
        Profile, Color, Size, SizeValue, Opacity, OpacityValue, Stabilizer, StabilizerValue, More,
        Tool, Splitter,
        DockTab, DockAction, LayerVisibility, LayerRow, LayerChevron, LayerLock, LayerClip, LayerList,
        LayerOpacity, LayerOpacityValue, BlendButton, AlphaLockBox, ClipBox, BlendItem,
        PopupBody, CatalogSearch, CatalogCategory, CatalogItem, DockBrush,
        FxSlider, FxCurve, FxChannel, FxCheck, FxApply, FxCancel,
        HueRing, SvSquare, ColorField, ColorPrevious,
    };
    struct HitResult {
        Hit kind = Hit::None;
        int index = 0;
        bool inDock = false;  // colour picker hits: the dock copy rather than the popup
        friend bool operator==(const HitResult&, const HitResult&) = default;
    };
    enum class Drag : std::uint8_t {
        None, Swallow, Size, Opacity, Stabilizer, LayerOpacity, Splitter, Hue, Sv, LayerRow, FxSlider, FxCurve,
    };
    enum class Field : std::uint8_t { None, Size, Opacity, Stabilizer, LayerOpacity, H, S, V, A, Hex, Search, LayerName };
    enum class Focus : std::uint8_t {
        Canvas, Profile, Color, Size, Opacity, Stabilizer, More, LayerList, LayerOpacity, Blend,
        Search, Categories, Items, H, S, V, A, Hex, BlendItems, FxParams,
    };
    /// One slider of the filter dialog.
    enum class FxParam : std::uint8_t { Radius, Gain, Threshold, ShiftX, ShiftY, Black, White, Mid, Edge, Amount };
    struct FxParamInfo {
        FxParam id;
        std::wstring_view label;
        double min, max;
        bool integer;
    };
    struct MenuEntry {
        std::wstring label;
        std::wstring shortcut;
        int action;  // Command, or 100 + Tool
        bool separatorAfter = false;
    };
    struct Menu {
        std::wstring title;
        std::vector<MenuEntry> entries;
        Rect titleRect;  // measured while drawing
    };
    struct PickerGeometry {
        Rect frame, square, current, previous;
        double cx = 0.0, cy = 0.0;
        std::array<Rect, 5> fields{};  // H, S, V, A, HEX
    };
    struct LayerRowInfo {
        std::uint64_t id = 0;
        int depth = 0;  // 0 for children of the root
    };
    struct DropTarget {
        bool valid = false;
        std::uint64_t parent = 0;
        std::size_t index = 0;
        std::uint64_t into = 0;  // a group row highlighted as the target
        double barY = 0.0;       // insertion bar, when not into a group
        double barX = 0.0;
    };
    struct PropertyRects {
        Rect opacity, blend, alphaLock, clip;
    };

    void relayout();
    [[nodiscard]] HitResult hitTest(double x, double y) const;
    [[nodiscard]] bool insideOverlay(const HitResult& h) const;
    void press(const HitResult& h, double x, double y, const UiState& state);
    void dragTo(double x, double y, const UiState& state);
    void endDrag(bool cancelled, const UiState& state);
    void setSlider(Drag which, double t, const UiState& state);

    void openPopup(Popup popup, const UiState& state);
    void closePopup(bool restore, const UiState& state);
    void startEdit(Field field, const UiState& state);
    void commitEdit(const UiState& state);
    void cancelEdit();
    void moveFocus(bool backwards, const UiState& state);
    void stepFocused(int direction, bool big, const UiState& state);
    void activateFocused(const UiState& state);
    void runMenuAction(int action, const UiState& state);
    void syncHsv(const UiState& state);
    void applyHsv(const UiState& state);
    [[nodiscard]] std::vector<std::size_t> catalogItems() const;
    [[nodiscard]] Rect popupRect() const;
    [[nodiscard]] Rect menuDropRect(int menu) const;
    [[nodiscard]] PickerGeometry picker(const Rect& frame) const;
    [[nodiscard]] Rect dockPickerFrame() const;
    [[nodiscard]] std::vector<LayerRowInfo> layerRows(const UiState& state) const;
    [[nodiscard]] Rect rowRect(std::size_t row) const;
    [[nodiscard]] Rect layerOpacityRect() const;
    [[nodiscard]] PropertyRects propertyRects() const;
    [[nodiscard]] Rect blendItemRect(int mode) const;
    [[nodiscard]] DropTarget dropTarget(double y, const UiState& state) const;
    [[nodiscard]] const core::LayerNode* activeLayer(const UiState& state) const;
    void selectRow(int direction, const UiState& state);
    void ensureRowVisible(std::size_t row);
    void clampLayerScroll(const UiState& state);
    void toggleFlag(std::uint64_t id, LayerFlag flag, const UiState& state);
    [[nodiscard]] Rect moreSliderRect() const;
    [[nodiscard]] bool menuEnabled(int action, const UiState& state) const;
    [[nodiscard]] std::wstring helpFor(const HitResult& h, const UiState& state) const;
    [[nodiscard]] std::wstring fieldText(Field f, const UiState& state) const;

    void drawSlider(UiRenderer& r, const Rect& control, std::wstring_view label, double t, Field field,
                    const std::wstring& valueText, bool hot, bool focused);
    void drawField(UiRenderer& r, const Rect& box, Field field, const std::wstring& text, bool focused);
    void drawPicker(UiRenderer& r, const PickerGeometry& g, const UiState& state, bool inDock);
    void drawCatalog(UiRenderer& r, const UiState& state);
    void drawLayers(UiRenderer& r, const UiState& state);
    void drawBlendPopup(UiRenderer& r, const UiState& state);
    void drawFxDialog(UiRenderer& r);

    // Filter dialog.
    void openFx(core::FxKind kind);
    void closeFx(bool apply);
    [[nodiscard]] std::vector<FxParamInfo> fxParams() const;
    [[nodiscard]] double fxValue(FxParam id) const;
    void setFxValue(FxParam id, double value);
    [[nodiscard]] Rect fxParamRect(std::size_t index) const;
    [[nodiscard]] Rect fxCurveRect() const;
    [[nodiscard]] Rect fxChannelRect(int channel) const;
    [[nodiscard]] Rect fxCheckRect() const;
    [[nodiscard]] Rect fxApplyRect() const;
    [[nodiscard]] Rect fxCancelRect() const;
    [[nodiscard]] double fxDialogHeight() const;
    void fxCurvePress(double x, double y);
    void fxCurveDrag(double x, double y);
    void drawMenus(UiRenderer& r, const UiState& state);
    void drawDock(UiRenderer& r, const UiState& state);
    void focusRing(UiRenderer& r, const Rect& box);

    UiHost& host_;
    Theme theme_;
    LayoutInput input_;
    std::optional<bool> dockShown_;  // F7 choice; default follows the width
    ShellLayout layout_;

    Popup popup_ = Popup::None;
    int menuOpen_ = -1;      // open dropdown, −1 none
    int menuItem_ = -1;      // highlighted entry
    bool altTap_ = false;    // Alt pressed with no other key yet: its release opens the menu
    std::vector<Menu> menus_;

    std::optional<std::pair<platform::PointerKind, std::uint32_t>> owner_;
    Drag drag_ = Drag::None;
    bool dragInDock_ = false;
    double dragRevert_ = 0.0;
    core::Rgb colorRevert_{};
    double alphaRevert_ = 1.0;
    double lastDragValue_ = 0.0;
    HitResult hover_{};

    Focus focus_ = Focus::Canvas;
    Field editField_ = Field::None;
    std::wstring editText_;
    bool editFresh_ = false;  // the first character replaces the whole value

    // Colour picker (popup and dock share the state).
    core::Hsv hsv_{};
    double lastHue_ = 0.0;
    core::Rgb colorAtOpen_{};
    double alphaAtOpen_ = 1.0;

    // Catalogue.
    std::wstring search_;
    int category_ = 0;
    std::optional<std::size_t> highlight_;  // index into catalog_
    std::vector<core::BrushProfile> catalog_;

    int dockTab_ = 0;  // 0 layers, 1 colour, 2 brushes

    // Layer list.
    std::set<std::uint64_t> collapsed_;  // groups whose children are hidden in the list
    double layerScroll_ = 0.0;
    std::uint64_t dragLayer_ = 0;  // row being pressed or dragged
    double dragStartY_ = 0.0;
    double dragY_ = 0.0;
    bool dragMoved_ = false;  // past the 4-DIP threshold: reordering
    std::uint64_t renameId_ = 0;
    std::uint64_t opacityLayer_ = 0;  // layer of the opacity drag
    std::uint64_t lastClickLayer_ = 0;
    double lastClickTime_ = -10.0;
    double pressTime_ = 0.0;
    int blendHighlight_ = 0;

    core::FxSettings fx_;
    int fxChannel_ = 0;     // tone curve: 0 Master, 1 R, 2 G, 3 B
    int fxPoint_ = -1;      // curve point being dragged
    int fxSlider_ = -1;     // slider being dragged
    int fxFocus_ = 0;       // focused slider for the keyboard
    double fxLastCurveClick_ = -10.0;
};

}  // namespace chroma::ui

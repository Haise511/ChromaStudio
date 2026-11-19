// ChromaStudio Engine · desktop · entry point and editor shell (Phase 4: interface).
//
//   wWinMain → COM → Per-Monitor V2 → log → PlatformWindow → presenter → low-latency loop
//
// Presenter: one OpenGL 3.3 core context (render_gl/GlContext) with the document tile renderer, a
// second tile renderer for brush previews and the chrome renderer (ui/UiRenderer). Without a usable
// context the panels and the paper are composed on the CPU and shown through a BGRA8 DIB; the
// interface and painting need the GPU path.
//
// Stroke path (spec §3, §4, §10.2): packet → document coordinates through the view (§2) →
// StrokeBuilder (stabiliser, §3.1 geometry, taper) → dabs → TileRenderer (stamp §4.1 into 128-px
// RGBA16F tiles) → on Up the tiles are read back into canonical RAM copies and become one history
// command. Esc, Alt+Tab or capture loss cancel the stroke and leave the history untouched.
//
// Interface (spec §11): ui/EditorUi lays out and draws the chrome and turns input into calls of the
// UiHost interface implemented here; pointer, keys and characters reach the chrome first.
//
// Layers (spec §7): the document is a tree of raster layers and groups (core/LayerTree). Every edit of
// it — add, duplicate, delete, move, properties — is one history command together with the tiles it
// affects (§10.2); the active layer travels with the tree but choosing one is not a command. Strokes
// write the active raster layer (Alpha Lock keeps its alpha); the window shows the GPU composite,
// while the pipette, thumbnails and PNG export use the CPU compositor (core/Compositor).
//
// Documents (spec §10.1, §10.3): .rawproj open and save with validation and atomic replacement,
// PNG export, "*" while the state differs from the saved revision, a save prompt before the
// document is closed or replaced. A lost GPU device is recreated and the tiles upload again from RAM.
//
// Filters (spec §9): Gaussian blur, tone curve, unsharp mask, chromatic aberration and line extraction run
// on the GPU per tile with their halo; the dialog's every change refreshes a preview held in GPU
// textures, Apply publishes the tiles (or a new layer) as one command, Cancel drops them.
//
// Vector lineart (spec §8): a brush stroke on a vector layer becomes an object of knots fitted to its
// final stamps, and the layer's pixels are replayed from the geometry (a cache in the history, not in
// the file). The V tool picks objects, drags knots, handles and the width diamond, inserts a knot on
// a double click and deletes with Delete; each gesture is one command, Esc restores the snapshot.
//
// Brushes (spec §13, §14): the seven built-in profiles plus the artist's library in
// %LOCALAPPDATA%\ChromaStudio\brushes; masks are found by SHA-256 among assets/textures, assets/materials
// and the library. Import reads §6 JSON, PresetFormat v1 (also as links and Base64) and IPBZ records.
//
// Raster operations (spec §10.1): lasso and magic wand selections (Shift add, Alt subtract, both
// intersect) that limit every write, fill, integer moves of a layer or its selection with a live
// preview, canvas crop, clipboard. Selection and canvas size are history state like the tree.
//
// Keys (spec §11.2): Ctrl+N/O/S, Ctrl+Shift+S, Ctrl+Shift+E · Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z · B brush · E eraser
//   I pipette · [ ] size ×2^∓1/8 · wheel zoom · Space+drag or middle drag pan · Ctrl+0 fit · Ctrl+1 1:1
//   F6 brush catalogue · F7 dock · F10 / Alt menu · Tab hide panels · Esc cancel · 1–7 brushes
//   Ctrl+Shift+N new layer · Alt+↑ / Alt+↓ move the active layer · S smudge · U blur · G fill · M select
//   Ctrl+T move · Ctrl+A / Ctrl+D / Ctrl+Shift+I selection · Ctrl+X / Ctrl+C / Ctrl+V clipboard
// Command line:  --cpu  --trace-input  --canvas=WxH (new documents, default 1920x1080)  [file.rawproj]
// Log:           %LOCALAPPDATA%\ChromaStudio\logs\desktop.log (spec §12)

#include "Clipboard.hpp"
#include "FileDialogs.hpp"
#include "PlatformWindow.hpp"

#include "core/BrushProfile.hpp"
#include "core/Color.hpp"
#include "core/Compositor.hpp"
#include "core/Fx.hpp"
#include "core/RasterOps.hpp"
#include "core/Selection.hpp"
#include "core/Vector.hpp"
#include "core/Half.hpp"
#include "core/LayerTree.hpp"
#include "core/Log.hpp"
#include "core/StrokeBuilder.hpp"
#include "core/View.hpp"
#include "project_io/Assets.hpp"
#include "project_io/BrushImport.hpp"
#include "project_io/Journal.hpp"
#include "project_io/PngExport.hpp"
#include "project_io/Project.hpp"
#include "render_gl/GlContext.hpp"
#include "render_gl/TileRenderer.hpp"
#include "ui/BrushPreview.hpp"
#include "ui/EditorUi.hpp"
#include "ui/UiRenderer.hpp"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <unordered_map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CHROMA_VERSION
#define CHROMA_VERSION "dev"
#endif

namespace {

using namespace chroma::platform;
namespace core = chroma::core;
namespace render = chroma::render;
namespace project = chroma::project;
namespace ui = chroma::ui;
namespace fs = std::filesystem;
using chroma::logf;
using Clock = std::chrono::steady_clock;

// ═════════════════════════════════════════════════════════════════════════════
// Text and log
// ═════════════════════════════════════════════════════════════════════════════

std::wstring toWide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr,
                        nullptr);
    return narrow;
}

std::ofstream& logStream() {
    static std::ofstream stream;
    return stream;
}

Clock::time_point logEpoch() {
    static const Clock::time_point epoch = Clock::now();
    return epoch;
}

void writeLogLine(std::string_view message) {
    const double seconds = std::chrono::duration<double>(Clock::now() - logEpoch()).count();
    const std::string line = std::format("[{:10.4f}] {}\n", seconds, message);
    OutputDebugStringW(toWide(line).c_str());
    if (std::ofstream& stream = logStream(); stream.is_open()) {
        stream << line;
        stream.flush();
    }
}

// Spec §12: data and log live under %LOCALAPPDATA%\ChromaStudio.
void openLog() {
    logEpoch();
    chroma::setLogSink(&writeLogLine);
    std::wstring base(32767, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    if (length == 0 || length >= base.size()) return;
    base.resize(length);
    const fs::path directory = fs::path(base) / L"ChromaStudio" / L"logs";
    std::error_code error;
    fs::create_directories(directory, error);
    logStream().open(directory / L"desktop.log", std::ios::out | std::ios::trunc | std::ios::binary);
}

std::string_view kindName(PointerKind kind) {
    switch (kind) {
    case PointerKind::Mouse: return "mouse";
    case PointerKind::Pen: return "pen";
    case PointerKind::Touch: return "touch";
    }
    return "?";
}

std::string_view phaseName(PointerPhase phase) {
    switch (phase) {
    case PointerPhase::Hover: return "hover";
    case PointerPhase::Down: return "down";
    case PointerPhase::Move: return "move";
    case PointerPhase::Up: return "up";
    case PointerPhase::Cancel: return "cancel";
    case PointerPhase::Leave: return "leave";
    }
    return "?";
}

std::string_view reasonName(CancelReason reason) {
    switch (reason) {
    case CancelReason::None: return "none";
    case CancelReason::System: return "system";
    case CancelReason::CaptureLost: return "capture lost";
    case CancelReason::Deactivated: return "deactivated";
    case CancelReason::Requested: return "requested";
    case CancelReason::Closing: return "closing";
    }
    return "?";
}

// Optional local textures (assets/textures) and papers (assets/materials) are copied next to the executable by CMake.
void logAssetInventory() {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    module.resize(length);
    const fs::path assets = fs::path(module).parent_path() / L"assets";

    const auto count = [](const fs::path& directory, const wchar_t* extension) {
        std::size_t files = 0;
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) && _wcsicmp(it->path().extension().c_str(), extension) == 0) ++files;
        }
        return files;
    };
    std::error_code error;
    if (!fs::is_directory(assets, error)) {
        logf("assets: missing next to the executable ({})", toUtf8(assets.wstring()));
        return;
    }
    logf("assets: {} PNG stamps in textures/, {} JPEG papers in materials/", count(assets / L"textures", L".png"),
         count(assets / L"materials", L".jpg"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Pixel geometry
// ═════════════════════════════════════════════════════════════════════════════

struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

Rgba8 toRgba8(const ui::Color& c) {
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {byte(c.r), byte(c.g), byte(c.b), byte(c.a)};
}

struct RectPx {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] int width() const noexcept { return x1 - x0; }
    [[nodiscard]] int height() const noexcept { return y1 - y0; }
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] bool contains(double x, double y) const noexcept { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    [[nodiscard]] RectPx intersect(const RectPx& o) const noexcept {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
};

// Spec §11: each edge is rounded on its own, so neighbouring regions share a boundary.
RectPx toPhysical(const ui::Rect& r, double scale) noexcept {
    const ui::EdgesPx e = ui::physicalEdges(r, scale);
    return {e.x0, e.y0, e.x1, e.y1};
}

constexpr Rgba8 kPaper{0xFF, 0xFF, 0xFF};
constexpr Rgba8 kCrosshair{0xE5, 0x48, 0x4D};

// ═════════════════════════════════════════════════════════════════════════════
// Presentation
// ═════════════════════════════════════════════════════════════════════════════

class Presenter {
public:
    virtual ~Presenter() = default;
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;
    virtual void resize(int clientWidth, int clientHeight) = 0;
    /// False when the device is lost.
    virtual bool beginFrame() = 0;
    /// Back buffer, client pixels, top-left origin.
    virtual void fill(const RectPx& rect, Rgba8 color) = 0;
    virtual void drawDocument(const render::DocumentFrame& frame, const render::LayerStack& stack) = 0;
    virtual void drawBrushCursor(const render::DocumentFrame& frame, double x, double y, double radiusPx) = 0;
    /// False when the device is lost.
    virtual bool present() = 0;
    /// The GPU tile renderer, or nullptr when painting is unavailable.
    [[nodiscard]] virtual render::TileRenderer* tiles() noexcept { return nullptr; }
    /// Brush previews with the real engine (same context).
    [[nodiscard]] virtual ui::BrushPreview* brushPreview() noexcept { return nullptr; }
    /// The chrome renderer, or nullptr when the interface is unavailable.
    [[nodiscard]] virtual ui::UiRenderer* chrome() noexcept { return nullptr; }
    [[nodiscard]] virtual std::unique_ptr<ui::UiImage> createImage() { return nullptr; }
};

class GlPresenter final : public Presenter {
public:
    static std::unique_ptr<Presenter> create(HWND hwnd, int documentWidth, int documentHeight, std::string& failure) {
        std::unique_ptr<chroma::gl::Context> context = chroma::gl::Context::create(hwnd, failure);
        if (!context) return nullptr;
        try {
            auto presenter = std::unique_ptr<GlPresenter>(new GlPresenter(std::move(context)));
            presenter->renderer_ = std::make_unique<render::TileRenderer>(*presenter->context_, documentWidth, documentHeight);
            presenter->preview_ = std::make_unique<ui::BrushPreview>(*presenter->context_);
            presenter->chrome_ = std::make_unique<ui::UiRenderer>(*presenter->context_);
            return presenter;
        } catch (const std::exception& error) {
            failure = error.what();
            return nullptr;
        }
    }

    [[nodiscard]] std::string_view label() const noexcept override { return context_->label(); }

    void resize(int clientWidth, int clientHeight) override {
        clientWidth_ = clientWidth;
        clientHeight_ = clientHeight;
    }

    bool beginFrame() override {
        if (context_->deviceLost()) return false;
        context_->api().bindFramebuffer(chroma::gl::kFramebuffer, 0);
        glViewport(0, 0, clientWidth_, clientHeight_);
        glDisable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        return true;
    }

    void fill(const RectPx& rect, Rgba8 color) override {
        const RectPx r = rect.intersect({0, 0, clientWidth_, clientHeight_});
        if (r.empty()) return;
        glDisable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        glScissor(r.x0, clientHeight_ - r.y1, r.width(), r.height());  // GL origin: bottom-left
        glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void drawDocument(const render::DocumentFrame& frame, const render::LayerStack& stack) override {
        renderer_->drawDocument(frame, stack);
    }

    void drawBrushCursor(const render::DocumentFrame& frame, double x, double y, double radiusPx) override {
        renderer_->drawBrushRing(frame, x, y, std::max(radiusPx, 1.5));
    }

    bool present() override { return context_->present(); }

    [[nodiscard]] render::TileRenderer* tiles() noexcept override { return renderer_.get(); }
    [[nodiscard]] ui::BrushPreview* brushPreview() noexcept override { return preview_.get(); }
    [[nodiscard]] ui::UiRenderer* chrome() noexcept override { return chrome_.get(); }
    [[nodiscard]] std::unique_ptr<ui::UiImage> createImage() override { return std::make_unique<ui::UiImage>(*context_); }

private:
    explicit GlPresenter(std::unique_ptr<chroma::gl::Context> context) : context_(std::move(context)) {}

    // Destroyed in reverse order, all while the context is still current.
    std::unique_ptr<chroma::gl::Context> context_;
    std::unique_ptr<render::TileRenderer> renderer_;
    std::unique_ptr<ui::BrushPreview> preview_;
    std::unique_ptr<ui::UiRenderer> chrome_;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
};

// CPU fallback: panels and paper as a BGRA8 frame shown through a DIB (spec §2), with a notice.
class GdiPresenter final : public Presenter {
public:
    explicit GdiPresenter(HWND hwnd) : hwnd_(hwnd) {}

    [[nodiscard]] std::string_view label() const noexcept override { return "CPU/DIB"; }

    void resize(int clientWidth, int clientHeight) override {
        width_ = std::max(clientWidth, 0);
        height_ = std::max(clientHeight, 0);
        frame_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u);
    }

    bool beginFrame() override { return true; }

    void fill(const RectPx& rect, Rgba8 color) override {
        const RectPx r = rect.intersect({0, 0, width_, height_});
        if (r.empty()) return;
        const std::uint32_t value = (std::uint32_t{color.a} << 24) | (std::uint32_t{color.r} << 16) |
                                    (std::uint32_t{color.g} << 8) | std::uint32_t{color.b};
        for (int y = r.y0; y < r.y1; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
            std::fill_n(frame_.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(r.x0)), r.width(),
                        value);
        }
    }

    void drawDocument(const render::DocumentFrame&, const render::LayerStack&) override {}

    void drawBrushCursor(const render::DocumentFrame& frame, double x, double y, double) override {
        const RectPx viewport{frame.viewportX0, frame.viewportY0, frame.viewportX1, frame.viewportY1};
        const int cx = static_cast<int>(std::floor(x));
        const int cy = static_cast<int>(std::floor(y));
        fill(RectPx{cx - 8, cy, cx + 9, cy + 1}.intersect(viewport), kCrosshair);
        fill(RectPx{cx, cy - 8, cx + 1, cy + 9}.intersect(viewport), kCrosshair);
    }

    bool present() override {
        if (frame_.empty()) return true;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;  // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(hwnd_);
        SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(width_), static_cast<DWORD>(height_), 0, 0, 0,
                          static_cast<UINT>(height_), frame_.data(), &info, DIB_RGB_COLORS);
        constexpr wchar_t kNotice[] = L"Нет OpenGL 3.3: интерфейс и рисование недоступны, видна только бумага";
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xE8, 0xE8, 0xEC));
        TextOutW(dc, 12, 8, kNotice, static_cast<int>(std::size(kNotice) - 1));
        ReleaseDC(hwnd_, dc);
        return true;
    }

private:
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint32_t> frame_;
};

// ═════════════════════════════════════════════════════════════════════════════
// Editor shell
// ═════════════════════════════════════════════════════════════════════════════

struct ShellOptions {
    bool forceCpu = false;
    bool traceInput = false;
    int documentWidth = 1920;  // size of new documents
    int documentHeight = 1080;
    fs::path openAtStart;
};

enum class Gesture : std::uint8_t { None, Stroke, Navigate, Pick, Lasso, Move, Crop, Tap, VectorEdit };

class EditorShell final : public WindowListener, public ui::UiHost {
public:
    explicit EditorShell(ShellOptions options)
        : options_(std::move(options)), ui_(*this), history_(kHistoryBudgetBytes, render::kTileBytes) {
        loadBrushes();
        const core::BrushProfile& first = brushes_.front();
        diameter_ = first.tip.diameter;
        opacity_ = first.ink.opacity;
        stabilizerMs_ = first.stabilizer.timeMs;
        stack_.tiles = [this](std::uint64_t layer, int tx, int ty) { return history_.get(core::TileKey{layer, tx, ty}); };
        resetDocument(project::emptyProject(options_.documentWidth, options_.documentHeight), {});
    }

    void attach(PlatformWindow& window) {
        window_ = &window;
        metrics_ = window.metrics();
        ui_.resize(metrics_.widthDip, metrics_.heightDip);
        createPresenter();
        applyGeometry();
        loggedDpi_ = metrics_.dpi;
        logGeometry();
        logf("brush: {} ({}), Ø {:.1f} px", currentBrush().name, currentBrush().id, diameter_);
        if (!options_.openAtStart.empty()) {
            openDocument(options_.openAtStart);  // logs the document itself
        } else {
            logDocument();
        }
    }

    [[nodiscard]] bool needsFrame() const noexcept { return dirty_ && presenter_ && !metrics_.minimized; }

    void renderFrame() {
        dirty_ = false;
        if (!presenter_ || metrics_.minimized || metrics_.widthPx <= 0 || metrics_.heightPx <= 0) return;
        if (!presenter_->beginFrame()) {
            recoverPresenter();
            return;
        }
        Presenter& p = *presenter_;
        if (fxDirty_) runFxPreview();
        if (thumbnails_.size() > displayTree().size()) {
            std::erase_if(thumbnails_, [this](const auto& entry) { return !displayTree().find(entry.first); });
        }
        const ui::UiState state = uiState();
        const ui::Theme& theme = ui_.theme();
        const ui::ShellLayout& layout = ui_.layout();
        const double scale = metrics_.scale();
        ui::UiRenderer* chrome = p.chrome();

        // Panels around the canvas.
        if (chrome) {
            chrome->begin(metrics_.widthPx, metrics_.heightPx, scale);
            ui_.draw(*chrome, state);
        } else {
            p.fill({0, 0, metrics_.widthPx, metrics_.heightPx}, toRgba8(theme.window));
            p.fill(toPhysical(layout.menu, scale), toRgba8(theme.menuBar));
            if (layout.optionsVisible) p.fill(toPhysical(layout.options, scale), toRgba8(theme.options));
            if (layout.toolbarVisible) p.fill(toPhysical(layout.toolbar, scale), toRgba8(theme.toolbar));
            if (layout.dockVisible) {
                p.fill(toPhysical(layout.splitter, scale), toRgba8(theme.splitter));
                p.fill(toPhysical(layout.dock, scale), toRgba8(theme.dock));
            }
            p.fill(toPhysical(layout.status, scale), toRgba8(theme.statusBar));
        }

        // The document.
        const RectPx canvas = canvasPx();
        const render::DocumentFrame frame = documentFrame();
        p.fill(canvas, toRgba8(theme.pasteboard));
        p.fill(paperRect(frame).intersect(canvas), kPaper);
        stack_.tree = &displayTree();
        p.drawDocument(frame, stack_);
        if (render::TileRenderer* tiles = p.tiles(); tiles && history_.meta().selection) {
            const bool movingSelection = gesture_ == Gesture::Move;
            tiles->drawSelection(frame, *history_.meta().selection, movingSelection ? floating_.dx : 0,
                                 movingSelection ? floating_.dy : 0);
        }
        const bool paintingTool = tool_ == ui::Tool::Brush || tool_ == ui::Tool::Eraser || tool_ == ui::Tool::Smudge ||
                                  tool_ == ui::Tool::Blur;
        if (cursor_ && cursor_->kind != PointerKind::Touch && paintingTool && !spaceHeld_ &&
            (gesture_ == Gesture::None || gesture_ == Gesture::Stroke) && !ui_.overlayOpen() &&
            canvas.contains(cursor_->xPx, cursor_->yPx)) {
            // §11: the hover contour shows the physical diameter D·z·dpi/96.
            p.drawBrushCursor(frame, cursor_->xPx, cursor_->yPx, 0.5 * diameter_ * frame.pixelsPerDocPx);
        }

        // Popups and menus over the canvas.
        if (chrome) {
            chrome->restoreState();
            drawToolOverlays(*chrome);
            drawVectorOverlay(*chrome);
            ui_.drawOverlays(*chrome, state);
        }
        if (!p.present()) recoverPresenter();
    }

    /// The window title: "name* — ChromaStudio" (spec §10.1: the asterisk marks unsaved changes).
    void updateTitle() {
        if (!window_) return;
        const std::wstring title = std::format(L"{}{} — ChromaStudio", documentName(), dirty() ? L"*" : L"");
        if (title != lastTitle_) {
            window_->setTitle(title);
            lastTitle_ = title;
        }
    }

    // ── WindowListener ──

    void onPointer(const PointerSample& s) override {
        if (options_.traceInput) traceSample(s);
        dirty_ = true;
        if (s.phase == PointerPhase::Down) notice_.clear();
        // The chrome sees the pointer first, unless a canvas gesture owns it.
        if (!owns(s) && ui_.pointer(s, uiState())) {
            if (s.phase == PointerPhase::Down) cursor_.reset();
            return;
        }

        switch (s.phase) {
        case PointerPhase::Hover:
            cursor_ = s;
            break;
        case PointerPhase::Leave:
            cursor_.reset();
            break;
        case PointerPhase::Down: {
            cursor_ = s;
            if (gesture_ != Gesture::None) break;  // §11.2: a catching-up pointer is ignored
            if (!canvasPx().contains(s.xPx, s.yPx)) break;
            const bool primary = (s.buttons & Button::Primary) != 0;
            if ((s.buttons & Button::Middle) || (primary && (spaceHeld_ || tool_ == ui::Tool::Hand))) {
                beginNavigate(s);
            } else if (primary && tool_ == ui::Tool::Pipette) {
                gesture_ = Gesture::Pick;
                owner_ = Owner{s.kind, s.pointerId};
                pickColor(s);
            } else if (primary && (tool_ == ui::Tool::Brush || tool_ == ui::Tool::Eraser || tool_ == ui::Tool::Smudge ||
                                   tool_ == ui::Tool::Blur)) {
                beginStroke(s);
            } else if (primary && tool_ == ui::Tool::Select) {
                beginLasso(s);
            } else if (primary && tool_ == ui::Tool::Fill) {
                gesture_ = Gesture::Tap;
                owner_ = Owner{s.kind, s.pointerId};
                fillAt(s);
            } else if (primary && tool_ == ui::Tool::Transform) {
                beginMove(s);
            } else if (primary && tool_ == ui::Tool::Crop) {
                beginCrop(s);
            } else if (primary && tool_ == ui::Tool::Vector) {
                beginVectorEdit(s);
            }
            break;
        }
        case PointerPhase::Move:
            cursor_ = s;
            if (!owns(s)) break;
            if (gesture_ == Gesture::Navigate) {
                view_.pan = navigatePan_ + (core::V{s.x, s.y} - navigateAnchor_);
            } else if (gesture_ == Gesture::Stroke) {
                continueStroke(s);
            } else if (gesture_ == Gesture::Pick) {
                pickColor(s);
            } else if (gesture_ == Gesture::Lasso) {
                const core::V doc = documentPoint(s);
                if (lasso_.empty() || std::hypot(doc.x - lasso_.back().x, doc.y - lasso_.back().y) >= 0.5) lasso_.push_back(doc);
            } else if (gesture_ == Gesture::Move) {
                updateMove(s);
            } else if (gesture_ == Gesture::Crop) {
                updateCrop(s);
            } else if (gesture_ == Gesture::VectorEdit) {
                updateVectorEdit(s);
            }
            break;
        case PointerPhase::Up:
            if (!owns(s)) break;
            if (gesture_ == Gesture::Stroke) endStroke(s);
            if (gesture_ == Gesture::Pick) pickColor(s);
            if (gesture_ == Gesture::Lasso) finishLasso(s);
            if (gesture_ == Gesture::Move) commitMove();
            if (gesture_ == Gesture::Crop) updateCrop(s);  // the frame stays until Enter or Esc
            if (gesture_ == Gesture::VectorEdit) commitVectorEdit();
            endGesture();
            break;
        case PointerPhase::Cancel:
            if (!owns(s)) break;
            if (gesture_ == Gesture::Stroke) cancelStroke(reasonName(s.cancelReason));
            if (gesture_ == Gesture::Lasso) lasso_.clear();
            if (gesture_ == Gesture::Move) cancelMove();
            if (gesture_ == Gesture::Crop) crop_.reset();
            if (gesture_ == Gesture::VectorEdit) vedit_.reset();  // Esc restores the snapshot (§8)
            endGesture();
            break;
        }
    }

    bool onKey(const KeyEvent& key) override {
        dirty_ = true;
        if (key.down) notice_.clear();
        if (ui_.key(key, uiState())) return true;
        if (key.virtualKey == VK_SPACE) {
            spaceHeld_ = key.down;  // Space + drag pans (§11.2)
            return true;
        }
        if (!key.down) return false;
        if (key.ctrl && !key.alt) {
            if (key.repeat && key.virtualKey != 'Z' && key.virtualKey != 'Y') return true;
            switch (key.virtualKey) {
            case 'Z': return key.shift ? redo() : undo();
            case 'Y': return redo();
            case '0': return fitView();
            case '1': return actualPixels();
            case 'N':
                if (key.shift) return layerCommand(ui::Command::NewLayer);
                return runCommand([this] { newDocument(); });
            case 'O': return runCommand([this] { openWithDialog(); });
            case 'S': return runCommand([this, asNew = key.shift] { asNew ? saveAs() : save(); });
            case 'E':
                if (!key.shift) return false;
                return runCommand([this] { exportWithDialog(); });
            case 'A': return editCommand(ui::Command::SelectAll);
            case 'D': return editCommand(ui::Command::Deselect);
            case 'I':
                if (!key.shift) return false;
                return editCommand(ui::Command::InvertSelection);
            case 'X': return editCommand(ui::Command::Cut);
            case 'C': return editCommand(ui::Command::Copy);
            case 'V': return editCommand(ui::Command::Paste);
            case 'T': selectTool(ui::Tool::Transform); return true;
            default: return false;
            }
        }
        if (key.alt) {
            // §11.1: Alt+↑ / Alt+↓ move the active layer by one position.
            if (key.virtualKey == VK_UP) return layerCommand(ui::Command::LayerUp);
            if (key.virtualKey == VK_DOWN) return layerCommand(ui::Command::LayerDown);
            return false;
        }
        switch (key.virtualKey) {
        case VK_ESCAPE:  // §11.2: Esc rolls back the active gesture
            if (window_) window_->cancelActiveContacts(CancelReason::Requested);
            if (gesture_ == Gesture::None && crop_) crop_.reset();
            return true;
        case VK_RETURN:
            if (crop_ && gesture_ == Gesture::None) applyCrop();
            return crop_.has_value() || tool_ == ui::Tool::Crop;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (tool_ != ui::Tool::Transform || gesture_ != Gesture::None) return false;
            nudge(key.virtualKey == VK_LEFT ? -1 : key.virtualKey == VK_RIGHT ? 1 : 0,
                  key.virtualKey == VK_UP ? -1 : key.virtualKey == VK_DOWN ? 1 : 0);
            return true;
        case 'G': selectTool(ui::Tool::Fill); return true;
        case 'V': selectTool(ui::Tool::Vector); return true;
        case VK_DELETE:
            if (tool_ != ui::Tool::Vector || gesture_ != Gesture::None) return false;
            return deleteVectorSelection();
        case 'M': selectTool(ui::Tool::Select); return true;
        case 'B': selectTool(ui::Tool::Brush); return true;
        case 'E': selectTool(ui::Tool::Eraser); return true;
        case 'S': selectTool(ui::Tool::Smudge); return true;
        case 'U': selectTool(ui::Tool::Blur); return true;
        case 'I': selectTool(ui::Tool::Pipette); return true;
        case VK_OEM_4:  // [
            setDiameter(diameter_ * std::exp2(-1.0 / 8.0));
            return true;
        case VK_OEM_6:  // ]
            setDiameter(diameter_ * std::exp2(1.0 / 8.0));
            return true;
        default:
            if (key.virtualKey >= '1' && key.virtualKey <= '7' && !key.repeat) {
                selectBrush(key.virtualKey - '1');
                return true;
            }
            return false;
        }
    }

    void onChar(char32_t codePoint) override {
        if (ui_.character(codePoint, uiState())) dirty_ = true;
    }

    bool onWheel(const WheelEvent& wheel) override {
        dirty_ = true;
        if (ui_.wheel(wheel, uiState())) return true;
        if (!wheel.ctrl && !canvasPx().contains(wheel.xPx, wheel.yPx)) return false;
        if (gesture_ != Gesture::None) return true;  // the view changes only between gestures
        // §2: zoom around the cursor; one notch is a quarter octave.
        const ui::Rect canvas = ui_.layout().canvas;
        const core::V anchor{wheel.x - canvas.x, wheel.y - canvas.y};
        view_.zoomAround(anchor, view_.zoom * std::exp2(wheel.notches / 4.0));
        return true;
    }

    HCURSOR cursorAt(double x, double y) override {
        static const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
        static const HCURSOR cross = LoadCursorW(nullptr, IDC_CROSS);
        static const HCURSOR move = LoadCursorW(nullptr, IDC_SIZEALL);
        static const HCURSOR resize = LoadCursorW(nullptr, IDC_SIZEWE);
        static const HCURSOR beam = LoadCursorW(nullptr, IDC_IBEAM);
        switch (ui_.cursorAt(x, y)) {
        case ui::CursorKind::Cross:
            return tool_ == ui::Tool::Hand || spaceHeld_ || gesture_ == Gesture::Navigate ? move : cross;
        case ui::CursorKind::Move: return move;
        case ui::CursorKind::ResizeWE: return resize;
        case ui::CursorKind::IBeam: return beam;
        case ui::CursorKind::Arrow: return arrow;
        }
        return arrow;
    }

    void onSystemSettingsChanged() override {
        ui_.setTheme(ui::currentTheme());  // §11.2: High Contrast is respected
        dirty_ = true;
    }

    void onMetricsChanged(const WindowMetrics& metrics) override {
        metrics_ = metrics;
        if (!metrics_.minimized && metrics_.widthPx > 0) ui_.resize(metrics_.widthDip, metrics_.heightDip);
        applyGeometry();
        if (presenter_ && metrics_.dpi != loggedDpi_) {
            loggedDpi_ = metrics_.dpi;
            logGeometry();
        }
    }

    void onPaint() override {
        // Also reached inside the modal size/move loop, where the main loop does not run.
        dirty_ = true;
        renderFrame();
    }

    void onActivationChanged(bool active) override {
        if (!active) {
            spaceHeld_ = false;  // the key-up goes to another window
            ui_.abandon(uiState());
        }
        dirty_ = true;
    }

    bool onCloseRequested() override {
        // Closing rolls back an unfinished gesture (§11.2), then asks about unsaved work.
        if (gesture_ != Gesture::None && window_) window_->cancelActiveContacts(CancelReason::Closing);
        if (!confirmDiscard()) return false;
        // Saved or discarded on purpose: nothing is left to recover.
        if (journal_) journal_->remove();
        journal_.reset();
        return true;
    }

    void onDestroy() override {
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) tiles->cancelStroke();
        history_.clear();  // tile versions go back to the pool while the context is alive
        thumbnails_.clear();
        presenter_.reset();  // the GL context goes before its window
        window_ = nullptr;
    }

    // ── UiHost ──

    void selectTool(ui::Tool tool) override {
        if (!ui::toolAvailable(tool)) return;
        if (tool != ui::Tool::Crop) crop_.reset();
        tool_ = tool;
        dirty_ = true;
    }

    void setTolerance(double unit) override {
        tolerance_ = std::clamp(unit, 0.0, 1.0);
        dirty_ = true;
    }

    void selectBrush(std::size_t index) override {
        const auto& catalog = brushes_;
        if (index >= catalog.size()) return;
        brushIndex_ = index;
        // §6: the quick controls reset to the profile when another preset is chosen.
        diameter_ = catalog[index].tip.diameter;
        opacity_ = catalog[index].ink.opacity;
        stabilizerMs_ = catalog[index].stabilizer.timeMs;
        if (tool_ != ui::Tool::Eraser && tool_ != ui::Tool::Smudge && tool_ != ui::Tool::Blur) tool_ = ui::Tool::Brush;
        dirty_ = true;
        logf("brush: {} ({}), Ø {:.1f} px{}", catalog[index].name, catalog[index].id, diameter_,
             core::isDry(catalog[index]) ? "" : ", wet (§5)");
    }

    void setDiameter(double px) override {
        diameter_ = std::clamp(px, 0.25, 2048.0);
        dirty_ = true;
    }

    void setOpacity(double unit) override {
        opacity_ = std::clamp(unit, 0.0, 1.0);
        dirty_ = true;
    }

    void setStabilizer(double ms) override {
        stabilizerMs_ = std::clamp(ms, 0.0, 200.0);
        dirty_ = true;
    }

    void setColor(const core::Rgb& srgb, double alpha) override {
        color_ = {std::clamp(srgb[0], 0.0, 1.0), std::clamp(srgb[1], 0.0, 1.0), std::clamp(srgb[2], 0.0, 1.0)};
        alpha_ = std::clamp(alpha, 0.0, 1.0);
        dirty_ = true;
    }

    void selectLayer(std::uint64_t id) override {
        core::LayerTree tree = history_.meta().layers;
        if (tree.setActive(id)) amendTree(std::move(tree));  // §10.2: not a command
        dirty_ = true;
    }

    void setLayerFlag(std::uint64_t id, ui::LayerFlag flag, bool on) override {
        core::LayerTree tree = history_.meta().layers;
        const core::LayerNode* node = tree.find(id);
        if (!node) return;
        core::LayerProps props = node->props;
        switch (flag) {
        case ui::LayerFlag::Visible: props.visible = on; break;
        case ui::LayerFlag::AlphaLock: props.alphaLock = on && node->kind == core::NodeKind::Raster; break;
        case ui::LayerFlag::ClipToBase: props.clipToBase = on; break;
        case ui::LayerFlag::Locked: props.locked = on; break;
        }
        if (tree.setProps(id, props)) commitTree(std::move(tree));
    }

    void setLayerOpacity(std::uint64_t id, double opacity, bool final) override {
        opacity = std::clamp(opacity, 0.0, 1.0);
        dirty_ = true;
        core::LayerTree tree = history_.meta().layers;
        const core::LayerNode* node = tree.find(id);
        if (!node) return;
        core::LayerProps props = node->props;
        props.opacity = opacity;
        tree.setProps(id, props);
        if (!final) {
            // While dragging only the display changes; the command is committed once at the end.
            if (tree == history_.meta().layers) {
                previewTree_.reset();
            } else {
                previewTree_ = std::move(tree);
            }
            return;
        }
        commitTree(std::move(tree));
    }

    void setLayerBlend(std::uint64_t id, int mode) override {
        core::LayerTree tree = history_.meta().layers;
        const core::LayerNode* node = tree.find(id);
        if (!node) return;
        core::LayerProps props = node->props;
        props.blendId = mode;
        if (tree.setProps(id, props)) commitTree(std::move(tree));
    }

    void renameLayer(std::uint64_t id, const std::string& utf8) override {
        core::LayerTree tree = history_.meta().layers;
        const core::LayerNode* node = tree.find(id);
        if (!node || utf8.empty()) return;
        core::LayerProps props = node->props;
        props.name = utf8;
        if (tree.setProps(id, props)) commitTree(std::move(tree));
    }

    void moveLayer(std::uint64_t id, std::uint64_t parent, std::size_t index) override {
        core::LayerTree tree = history_.meta().layers;
        if (tree.move(id, parent, index)) {
            commitTree(std::move(tree));
        } else {
            showNotice(L"Слой нельзя переместить сюда: внутрь самого себя или глубже 64 уровней");
        }
    }

    void fxChanged(const core::FxSettings& settings) override {
        fxPending_ = settings;
        fxDirty_ = true;
        dirty_ = true;
    }

    void fxApply(const core::FxSettings& settings) override {
        fxPending_ = settings;
        runFxPreview();  // what is published is exactly what was previewed
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles || !tiles->fxActive() || fxTarget_ == 0) {
            fxCancel();
            return;
        }
        const std::uint64_t target = fxTarget_;
        std::vector<render::TileChange> changes =
            tiles->commitFx([this, target](int tx, int ty) { return history_.get(core::TileKey{target, tx, ty}); });
        const std::size_t count = changes.size();
        if (fxTree_) {
            commitTree(std::move(*fxTree_), std::move(changes));  // "to a new layer": the layer and its tiles at once
        } else {
            previewTree_.reset();
            history_.commit(std::move(changes));
        }
        logf("filter «{}»: {} tiles", core::fxName(settings.kind), count);
        fxPending_.reset();
        fxTree_.reset();
        fxTarget_ = 0;
        dirty_ = true;
    }

    void fxCancel() override {
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) tiles->cancelFx();
        if (fxTree_) previewTree_.reset();
        fxPending_.reset();
        fxTree_.reset();
        fxTarget_ = 0;
        fxDirty_ = false;
        dirty_ = true;
    }

    void run(ui::Command command) override {
        dirty_ = true;
        switch (command) {
        case ui::Command::NewDocument: runCommand([this] { newDocument(); }); break;
        case ui::Command::Open: runCommand([this] { openWithDialog(); }); break;
        case ui::Command::Save: runCommand([this] { save(); }); break;
        case ui::Command::SaveAs: runCommand([this] { saveAs(); }); break;
        case ui::Command::ExportPng: runCommand([this] { exportWithDialog(); }); break;
        case ui::Command::Exit:
            if (window_) window_->requestClose();
            break;
        case ui::Command::Undo: undo(); break;
        case ui::Command::Redo: redo(); break;
        case ui::Command::FitView: fitView(); break;
        case ui::Command::ActualPixels: actualPixels(); break;
        case ui::Command::NewLayer:
        case ui::Command::NewGroup:
        case ui::Command::DuplicateLayer:
        case ui::Command::DeleteLayer:
        case ui::Command::LayerUp:
        case ui::Command::LayerDown:
        case ui::Command::NewVectorLayer:
        case ui::Command::RasterizeLayer: layerCommand(command); break;
        case ui::Command::Cut:
        case ui::Command::Copy:
        case ui::Command::Paste:
        case ui::Command::SelectAll:
        case ui::Command::Deselect:
        case ui::Command::InvertSelection: editCommand(command); break;
        case ui::Command::ImportBrush: runCommand([this] { importBrushFile(); }); break;
        case ui::Command::ImportImage:
            runCommand([this] {
                if (const auto path = chroma::desktop::askImportImage(owner(), path_)) importPicture(*path);
            });
            break;
        case ui::Command::PasteBrush: runCommand([this] { importBrushFromClipboard(); }); break;
        case ui::Command::About: {
            const std::wstring text = std::format(L"ChromaStudio Engine {}\nНастольный редактор для художников.\n\n"
                                                  L"Журнал: %LOCALAPPDATA%\\ChromaStudio\\logs\\desktop.log",
                                                  toWide(CHROMA_VERSION));
            MessageBoxW(owner(), text.c_str(), L"О программе", MB_OK | MB_ICONINFORMATION);
            break;
        }
        }
    }

    // §11.1: the catalogue preview uses the same engine as the canvas.
    void drawBrushPreview(ui::UiRenderer& r, std::size_t index, const ui::Rect& area) override {
        if (ui::BrushPreview* preview = presenter_ ? presenter_->brushPreview() : nullptr; preview && index < brushes_.size()) {
            preview->draw(r, brushes_[index], area, color_, alpha_);
        } else {
            r.restoreState();
        }
    }

    void drawLayerThumbnail(ui::UiRenderer& r, std::uint64_t id, const ui::Rect& area) override {
        if (!presenter_) return;
        const double scale = metrics_.scale();
        // Fit the document's aspect into the box.
        const double aspect = static_cast<double>(docHeight_) / docWidth_;
        double w = area.w;
        double h = area.w * aspect;
        if (h > area.h) {
            h = area.h;
            w = area.h / aspect;
        }
        const int pw = std::max(1, static_cast<int>(std::lround(w * scale)));
        const int ph = std::max(1, static_cast<int>(std::lround(h * scale)));
        Thumbnail& thumb = thumbnails_[id];
        if (!thumb.image) thumb.image = presenter_->createImage();
        if (!thumb.image) return;
        const ThumbnailKey key{history_.revision(), pw, ph};
        if (!(thumb.key == key)) {
            // The layer's own pixels over white, nearest sample per thumbnail pixel.
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 4);
            for (int j = 0; j < ph; ++j) {
                for (int i = 0; i < pw; ++i) {
                    const int x = std::min(docWidth_ - 1, static_cast<int>((i + 0.5) * docWidth_ / pw));
                    const int y = std::min(docHeight_ - 1, static_cast<int>((j + 0.5) * docHeight_ / ph));
                    const core::Pixel p = layerPixel(id, x, y);
                    std::uint8_t* out = &rgba[(static_cast<std::size_t>(j) * static_cast<std::size_t>(pw) + i) * 4];
                    out[0] = core::toByte(core::linearToSrgb(p.r + 1.0 - p.a));
                    out[1] = core::toByte(core::linearToSrgb(p.g + 1.0 - p.a));
                    out[2] = core::toByte(core::linearToSrgb(p.b + 1.0 - p.a));
                    out[3] = 255;
                }
            }
            thumb.image->update(pw, ph, rgba.data());
            thumb.key = key;
        }
        r.image(*thumb.image, {area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h});
    }

private:
    struct Owner {
        PointerKind kind;
        std::uint32_t pointerId;
    };

    struct ThumbnailKey {
        std::uint64_t revision = 0;
        int width = 0;
        int height = 0;
        friend bool operator==(const ThumbnailKey&, const ThumbnailKey&) = default;
    };

    struct Thumbnail {
        std::unique_ptr<ui::UiImage> image;
        ThumbnailKey key;
    };

    static constexpr std::size_t kHistoryBudgetBytes = std::size_t{256} << 20;  // spec §10.2
    static constexpr int kMaxDeviceLosses = 3;
    static constexpr int kBlurRadius = 6;  // §9.2 initial radius

    [[nodiscard]] const core::BrushProfile& currentBrush() const { return brushes_[std::min(brushIndex_, brushes_.size() - 1)]; }

    /// The tree as shown: the history's, or a preview while a layer opacity is being dragged.
    [[nodiscard]] const core::LayerTree& displayTree() const { return previewTree_ ? *previewTree_ : history_.meta().layers; }

    [[nodiscard]] const core::LayerNode* activeLayer() const { return history_.meta().layers.find(history_.meta().layers.active()); }

    [[nodiscard]] ui::UiState uiState() const {
        ui::UiState s;
        s.tool = tool_;
        s.brush = brushIndex_;
        s.diameter = diameter_;
        s.opacity = opacity_;
        s.stabilizerMs = stabilizerMs_;
        s.tolerance = tolerance_;
        s.color = color_;
        s.alpha = alpha_;
        s.canUndo = history_.cursor() > 0;
        s.canRedo = history_.cursor() < history_.size();
        s.painting = gesture_ != Gesture::None;
        s.layers = &displayTree();
        s.status = statusText();
        s.notice = notice_;
        return s;
    }

    [[nodiscard]] std::wstring statusText() const {
        std::wstring text = std::format(L"{} × {} px · {:.0f} %", docWidth_, docHeight_, view_.zoom * metrics_.scale() * 100.0);
        if (cursor_ && canvasPx().contains(cursor_->xPx, cursor_->yPx)) {
            const ui::Rect canvas = ui_.layout().canvas;
            const core::V doc = view_.toDocument({cursor_->x - canvas.x, cursor_->y - canvas.y});
            text += std::format(L" · {:.0f}, {:.0f}", std::floor(doc.x), std::floor(doc.y));
        }
        if (crop_) text += std::format(L" · кадр {} × {} (Enter)", crop_->width(), crop_->height());
        text += std::format(L" · отмена {}/{}", history_.cursor(), history_.size());
        if (presenter_) text += std::format(L" · {}", toWide(presenter_->label()));
        return text;
    }

    void showNotice(std::wstring text) {
        logf("notice: {}", toUtf8(text));
        notice_ = std::move(text);
        dirty_ = true;
    }

    /// Publishes an edited tree, with the tiles it affects, as one history command (§10.2).
    void commitTree(core::LayerTree tree, std::vector<render::TileChange> changes = {}) {
        dirty_ = true;
        previewTree_.reset();
        if (gesture_ == Gesture::Stroke) return;  // §11.2: document commands wait for Up
        core::DocumentMeta meta = history_.meta();
        meta.layers = std::move(tree);
        history_.commit(std::move(changes), std::move(meta));
    }

    /// The active layer changes without a command (§10.2).
    void amendTree(core::LayerTree tree) {
        core::DocumentMeta meta = history_.meta();
        meta.layers = std::move(tree);
        history_.amendMeta(std::move(meta));
    }

    /// The committed tiles of one raster layer.
    [[nodiscard]] std::vector<std::pair<core::TileKey, render::TileRef>> tilesOf(std::uint64_t layer) const {
        std::vector<std::pair<core::TileKey, render::TileRef>> out;
        history_.forEachTile([&](const core::TileKey& key, const render::TileRef& ref) {
            if (key.layer == layer) out.emplace_back(key, ref);
        });
        return out;
    }

    bool layerCommand(ui::Command command) {
        dirty_ = true;
        if (gesture_ != Gesture::None) return true;  // §11.2: wait for Up
        core::LayerTree tree = history_.meta().layers;
        const std::uint64_t active = tree.active();
        std::vector<render::TileChange> changes;
        switch (command) {
        case ui::Command::NewLayer:
            if (!tree.addRaster()) return showNotice(L"Достигнут предел 4096 слоёв или глубины 64"), true;
            break;
        case ui::Command::NewGroup:
            if (!tree.addGroup()) return showNotice(L"Достигнут предел 4096 слоёв или глубины 64"), true;
            break;
        case ui::Command::NewVectorLayer:
            if (!tree.addVector()) return showNotice(L"Достигнут предел 4096 слоёв или глубины 64"), true;
            break;
        case ui::Command::RasterizeLayer: {
            // The cached pixels stay as a raster layer; the objects go (one command).
            const core::LayerNode* node = tree.find(active);
            if (!node || node->kind != core::NodeKind::Vector) return showNotice(L"Активный слой не векторный"), true;
            core::DocumentMeta meta = history_.meta();
            meta.layers.rasterize(active);
            meta.vectors.erase(active);
            history_.commit({}, std::move(meta));
            return true;
        }
        case ui::Command::DuplicateLayer: {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> copies;
            if (!tree.duplicate(active, copies)) return showNotice(L"Достигнут предел 4096 слоёв"), true;
            duplicated_ = copies;
            // The copy shares the tile versions of its source (§10.2: equal content is stored once).
            for (const auto& [source, copy] : copies) {
                for (const auto& [key, ref] : tilesOf(source)) changes.push_back({core::TileKey{copy, key.tx, key.ty}, nullptr, ref});
            }
            break;
        }
        case ui::Command::DeleteLayer: {
            std::vector<std::uint64_t> removed;
            if (!tree.remove(active, removed)) return true;
            render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
            for (const std::uint64_t layer : removed) {
                for (const auto& [key, ref] : tilesOf(layer)) {
                    changes.push_back({key, ref, nullptr});
                    if (tiles) tiles->evict(*ref);  // kept in RAM for undo, not displayed
                }
            }
            break;
        }
        case ui::Command::LayerUp:
        case ui::Command::LayerDown:
            if (!tree.step(active, command == ui::Command::LayerUp ? 1 : -1)) return true;
            break;
        default: return false;
        }
        const std::size_t tileCount = changes.size();
        // Vector objects follow their layers: removed with them, copied with fresh IDs.
        core::DocumentMeta meta = history_.meta();
        std::erase_if(meta.vectors, [&tree](const auto& entry) { return !tree.find(entry.first); });
        for (const std::uint64_t id : tree.rasters()) {
            const core::LayerNode* node = tree.find(id);
            if (node->kind == core::NodeKind::Vector && !meta.vectors.contains(id)) {
                meta.vectors[id];  // a new or duplicated vector layer
                for (const auto& [source, copy] : duplicated_) {
                    if (copy != id || !history_.meta().vectors.contains(source)) continue;
                    auto objects = history_.meta().vectors.at(source);
                    for (core::VectorObject& o : objects) {
                        o.id = tree.reserveId();
                        for (core::Knot& k : o.knots) k.id = tree.reserveId();
                    }
                    meta.vectors[id] = std::move(objects);
                }
            }
        }
        duplicated_.clear();
        meta.layers = std::move(tree);
        previewTree_.reset();
        if (gesture_ != Gesture::Stroke) history_.commit(std::move(changes), std::move(meta));
        dirty_ = true;
        const core::LayerNode* now = activeLayer();
        logf("layers: {} nodes, active «{}»{}", history_.meta().layers.size(), now ? now->props.name : std::string("—"),
             tileCount ? std::format(", {} tiles", tileCount) : std::string());
        return true;
    }

    // ── Strokes ──

    [[nodiscard]] core::StrokeSample strokeSample(const PointerSample& s) const {
        const ui::Rect canvas = ui_.layout().canvas;
        const core::V view{s.x - canvas.x, s.y - canvas.y};
        return {s.timeSeconds * 1000.0, view_.toDocument(view), s.pressure, s.tiltX, s.tiltY};
    }

    [[nodiscard]] render::BrushSetup brushSetup(const core::BrushProfile& profile, bool erase) const {
        render::BrushSetup setup =
            render::brushSetupFor(profile, core::premultipliedLinear(color_[0], color_[1], color_[2], alpha_), erase);
        if (const core::LayerNode* layer = activeLayer()) {
            setup.layer = layer->id;
            setup.alphaLock = layer->props.alphaLock;
        }
        resolveMasks(setup, profile);
        return setup;
    }

    /// The profile as the quick controls set it (spec §6: the sliders override the profile).
    [[nodiscard]] core::BrushProfile effectiveProfile() const {
        core::BrushProfile profile = currentBrush();
        profile.ink.opacity = opacity_;
        profile.stabilizer.timeMs = stabilizerMs_;
        if (profile.stabilizer.method == core::StabilizerMethod::Off && stabilizerMs_ > 0) {
            profile.stabilizer.method = core::StabilizerMethod::Ema;
        }
        return profile;
    }

    void beginStroke(const PointerSample& s) {
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles) {
            if (!warnedNoGpu_) logf("painting needs the GPU path; the CPU presenter only shows the paper");
            warnedNoGpu_ = true;
            return;
        }
        const core::LayerTree& tree = history_.meta().layers;
        const core::LayerNode* layer = activeLayer();
        const bool erase = tool_ == ui::Tool::Eraser || (s.buttons & Button::Eraser) != 0;
        if (!layer || layer->kind == core::NodeKind::Group) {
            showNotice(L"Выбрана группа: рисовать можно только на растровом или векторном слое");
            return;
        }
        if (tree.effectivelyLocked(layer->id)) {
            showNotice(std::format(L"Слой «{}» заблокирован", toWide(layer->props.name)));  // §7: no pixel changes
            return;
        }
        if (layer->kind == core::NodeKind::Vector) {
            // §8: vector objects keep a dry profile; reading tools need a raster copy.
            if (erase || tool_ != ui::Tool::Brush) {
                showNotice(L"Векторный слой: стирайте узлы инструментом V, а палец и размытие работают после «Слой → Растеризовать»");
                return;
            }
            if (!core::isDry(currentBrush())) {
                showNotice(L"Векторный штрих хранит сухую кисть: выберите перо, аэрограф, фактуру или скринтон");
                return;
            }
        }
        if (!tree.effectivelyVisible(layer->id)) {
            showNotice(std::format(L"Слой «{}» скрыт", toWide(layer->props.name)));
            return;
        }
        if (erase && layer->props.alphaLock) {
            showNotice(L"Ластик не действует на слое с Alpha Lock");  // §7
            return;
        }
        // Snapshot of the effective profile at stroke start (§6); the eraser end acts as eraser (§3).
        const core::BrushProfile profile = effectiveProfile();
        builder_.emplace(profile, diameter_);
        const auto& selection = history_.meta().selection;
        tiles->setSelection(selection ? &*selection : nullptr);  // §10.1: the selection limits writing
        const std::uint64_t target = layer->id;
        render::BrushSetup setup = brushSetup(profile, erase);
        if (!erase && (tool_ == ui::Tool::Smudge || tool_ == ui::Tool::Blur)) {
            // §5: the finger and the blur use the current tip; finger defaults b = 1, d = 0, e = 0.7.
            setup.stroke = tool_ == ui::Tool::Smudge ? render::StrokeKind::Smudge : render::StrokeKind::Blur;
            setup.maxMode = false;
            setup.blending = 1.0f;
            setup.dilution = 0.0f;
            setup.bleed = 0.7f;
            setup.strength = 1.0f;
            setup.blurRadius = kBlurRadius;
        }
        tiles->beginStroke(setup, [this, target](int tx, int ty) { return history_.get(core::TileKey{target, tx, ty}); });
        gesture_ = Gesture::Stroke;
        owner_ = Owner{s.kind, s.pointerId};
        strokeErase_ = erase;
        strokeStarted_ = Clock::now();
        strokeVector_ = layer->kind == core::NodeKind::Vector;
        strokeProfile_ = profile;
        strokeDabs_.clear();
        continueStroke(s);
    }

    void continueStroke(const PointerSample& s) {
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles || !builder_) return;
        builder_->add(strokeSample(s), finalDabs_);
        tiles->applyDabs(finalDabs_);
        dabCount_ += finalDabs_.size();
        if (strokeVector_) strokeDabs_.insert(strokeDabs_.end(), finalDabs_.begin(), finalDabs_.end());
        finalDabs_.clear();
        builder_->tail(tailDabs_);
        tiles->setTail(tailDabs_);
    }

    void endStroke(const PointerSample& s) {
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles || !builder_) return;
        core::StrokeResult result = builder_->finish(strokeSample(s));
        if (strokeVector_) {
            // §8: the drawn stamps become knots; the pixels are replayed from the new object.
            if (result.replay) {
                strokeDabs_ = std::move(result.dabs);
            } else {
                strokeDabs_.insert(strokeDabs_.end(), result.dabs.begin(), result.dabs.end());
            }
            tiles->cancelStroke();
            builder_.reset();
            dabCount_ = 0;
            addVectorObject();
            return;
        }
        if (result.replay) {
            tiles->replay(result.dabs);
            dabCount_ = result.dabs.size();
        } else {
            tiles->applyDabs(result.dabs);
            dabCount_ += result.dabs.size();
        }
        std::vector<render::TileChange> changes = tiles->commitStroke();
        const std::size_t tileCount = changes.size();
        // Versions replaced by this command stay in RAM for undo; their GPU copies are not needed.
        for (const render::TileChange& c : changes) {
            if (c.before && c.before != c.after) tiles->evict(*c.before);
        }
        const bool committed = history_.commit(std::move(changes));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - strokeStarted_).count();
        logf("stroke {}: {} dabs, L = {:.1f} px, lift {:.0f} px/s, {} tiles{}{}, {:.0f} ms; history {}/{} ({:.1f} MiB)",
             strokeErase_ ? "erased" : "committed", dabCount_, result.length, result.liftVelocity, tileCount,
             result.replay ? ", replayed with the final taper" : "", committed ? "" : " (no change)", ms,
             history_.cursor(), history_.size(), history_.bytes() / 1048576.0);
        builder_.reset();
        dabCount_ = 0;
    }

    void cancelStroke(std::string_view reason) {
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) tiles->cancelStroke();
        logf("stroke cancelled ({}): history unchanged at {}/{}", reason, history_.cursor(), history_.size());
        builder_.reset();
        dabCount_ = 0;
    }

    /// Linear premultiplied pixel of one raster layer at document (x, y).
    [[nodiscard]] core::Pixel layerPixel(std::uint64_t layer, int x, int y) const {
        const render::TileRef tile = history_.get(core::TileKey{layer, x / render::kTileSize, y / render::kTileSize});
        if (!tile) return {};
        const std::size_t i =
            (static_cast<std::size_t>(y % render::kTileSize) * render::kTileSize + static_cast<std::size_t>(x % render::kTileSize)) * 4;
        const auto& h = tile->data().halfs;
        return {core::halfToFloat(h[i]), core::halfToFloat(h[i + 1]), core::halfToFloat(h[i + 2]), core::halfToFloat(h[i + 3])};
    }

    /// The composite of the visible tree at document (x, y), without paper (§7).
    [[nodiscard]] core::Pixel compositePixel(int x, int y) const {
        core::Pixel out;
        core::compose(displayTree(), std::span<core::Pixel>(&out, 1), [&](std::uint64_t layer, std::span<core::Pixel> px) {
            px[0] = layerPixel(layer, x, y);
            return px[0].a > 0.0f;
        });
        return out;
    }

    // §10.1: the pipette reads the composite without UI and checkerboard; a pixel with alpha zero is
    // RGBA zero and leaves the colour as it is.
    void pickColor(const PointerSample& s) {
        const ui::Rect canvas = ui_.layout().canvas;
        const core::V doc = view_.toDocument({s.x - canvas.x, s.y - canvas.y});
        const int x = static_cast<int>(std::floor(doc.x));
        const int y = static_cast<int>(std::floor(doc.y));
        if (x < 0 || y < 0 || x >= docWidth_ || y >= docHeight_) return;
        const core::Pixel p = compositePixel(x, y);
        if (p.a <= 0.0f) return;
        setColor({core::linearToSrgb(p.r / p.a), core::linearToSrgb(p.g / p.a), core::linearToSrgb(p.b / p.a)}, p.a);
    }

public:
    // ── Recovery journal (§10.3) ──

    static constexpr auto kJournalPeriod = std::chrono::seconds(30);
    static constexpr std::uint64_t kJournalRotateBytes = std::uint64_t{512} << 20;

    [[nodiscard]] static fs::path recoveryDirectory() { return dataDirectory() / L"recovery"; }

    [[nodiscard]] fs::path journalFile() const {
        return recoveryDirectory() / std::format(L"{}-{}.journal", toWide(sessionId_), journalGeneration_);
    }

    [[nodiscard]] project::JournalHeader journalHeader() const {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return {sessionId_, toUtf8(documentName()), journalBase_, journalBaseSha_,
                std::chrono::duration_cast<std::chrono::seconds>(now).count()};
    }

    /// The document now equals `base` (a file just opened or saved) or is new: the journal restarts.
    void restartJournal(const fs::path& base) {
        if (journal_) journal_->remove();
        journal_.reset();
        journalBase_.clear();
        journalBaseSha_.clear();
        journalState_ = {};
        if (!base.empty()) {
            if (const auto bytes = project::readFileBytes(base)) {
                journalBase_ = toUtf8(base.wstring());
                journalBaseSha_ = project::sha256Hex(*bytes);
                journalState_ = project::JournalState::of(snapshot());
            }
        }
        journalRevision_ = history_.revision();
    }

    /// From the message loop: one record every 30 s while there are unsaved changes.
    void maintainJournal() {
        const Clock::time_point now = Clock::now();
        if (now < journalDue_) return;
        journalDue_ = now + kJournalPeriod;
        writeJournal(false);
    }

    [[nodiscard]] DWORD idleTimeoutMs() const {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(journalDue_ - Clock::now()).count();
        return static_cast<DWORD>(std::clamp<long long>(left, 0, 30'000));
    }

    void writeJournal(bool force) {
        if (!force && history_.revision() == journalRevision_) return;
        if (gesture_ != Gesture::None || fxPending_) {
            journalDue_ = Clock::now() + std::chrono::seconds(2);  // after the gesture or the filter dialog
            return;
        }
        if (!dirty()) {
            // Back at the saved revision: nothing to recover.
            if (journal_) journal_->remove();
            journal_.reset();
            journalState_ = journalBase_.empty() ? project::JournalState{} : project::JournalState::of(snapshot());
            journalRevision_ = history_.revision();
            return;
        }
        std::string error;
        if (!journal_) {
            ++journalGeneration_;
            journal_ = project::Journal::create(journalFile(), journalHeader(), error);
            if (!journal_) {
                warnJournal(error);
                return;
            }
        }
        const Clock::time_point start = Clock::now();
        const project::ProjectSnapshot s = snapshot();
        auto [changes, next] = journalState_.diff(s);
        if (!changes.empty()) {
            error = journal_->append(changes);
            if (!error.empty()) {
                warnJournal(error);
                return;
            }
        }
        journalState_ = std::move(next);
        journalRevision_ = history_.revision();
        logf("recovery journal: record {} with {} changes, {:.1f} MiB in all, {:.0f} ms", journal_->records(), changes.size(),
             static_cast<double>(journal_->bytes()) / (1 << 20),
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (journal_->bytes() > kJournalRotateBytes) rotateJournal(s);
    }

    /// A long session: a new generation from the full snapshot is written before the old one goes.
    void rotateJournal(const project::ProjectSnapshot& s) {
        std::string error;
        ++journalGeneration_;
        const std::string base = std::exchange(journalBase_, {});
        const std::string sha = std::exchange(journalBaseSha_, {});
        auto fresh = project::Journal::create(journalFile(), journalHeader(), error);
        auto [changes, next] = project::JournalState{}.diff(s);
        if (!fresh || !fresh->append(changes).empty()) {
            if (fresh) fresh->remove();
            journalBase_ = base;
            journalBaseSha_ = sha;
            return;
        }
        journal_->remove();
        journal_ = std::move(fresh);
        journalState_ = std::move(next);
    }

    void warnJournal(const std::string& error) {
        logf("recovery journal: {}", error);
        if (!journalWarned_) showNotice(L"Журнал восстановления не записан: несохранённые изменения не защищены от сбоя");
        journalWarned_ = true;
    }

    [[nodiscard]] static std::wstring localTime(std::int64_t unixSeconds) {
        const auto ticks = static_cast<std::uint64_t>(unixSeconds + 11'644'473'600LL) * 10'000'000ULL;
        FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        SYSTEMTIME u{}, l{};
        if (!FileTimeToSystemTime(&utc, &u) || !SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) return L"?";
        return std::format(L"{:02}.{:02}.{} {:02}:{:02}", l.wDay, l.wMonth, l.wYear, l.wHour, l.wMinute);
    }

    /// At start-up: work left by a session that ended without saving (§10.3) opens as a modified copy.
    void offerRecovery() {
        std::error_code ec;
        std::vector<project::RecoveredJournal> found;
        for (const auto& entry : fs::directory_iterator(recoveryDirectory(), ec)) {
            if (entry.path().extension() != L".journal") continue;
            std::string error;
            auto journal = project::readJournal(entry.path(), error);
            if (!journal) {
                logf("recovery: {} skipped: {}", toUtf8(entry.path().filename().wstring()), error);
                continue;
            }
            if (journal->records.empty()) {
                DeleteFileW(entry.path().c_str());  // nothing complete was recorded
                continue;
            }
            found.push_back(std::move(*journal));
        }
        if (found.empty()) return;
        // The newest first; one document at a time, the others wait for the next start.
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.header.created > b.header.created; });
        const project::RecoveredJournal& j = found.front();
        const std::wstring question = std::format(
            L"Найдена несохранённая работа «{}» от {}{}.\n\nДа — открыть восстановленную копию.\nНет — удалить эти изменения.\n"
            L"Отмена — спросить при следующем запуске.",
            toWide(j.header.name), localTime(j.header.created),
            j.tail.empty() ? L"" : L" (последняя неполная запись пропущена)");
        logf("recovery: {} — {} records{}", toUtf8(j.path.filename().wstring()), j.records.size(),
             j.tail.empty() ? "" : ", tail: " + j.tail);
        switch (MessageBoxW(owner(), question.c_str(), L"Восстановление", MB_YESNOCANCEL | MB_ICONQUESTION)) {
        case IDYES: {
            const project::LoadResult r = project::replayJournal(j);
            std::string treeError;
            if (!r.snapshot || !project::layerTree(*r.snapshot, treeError)) {
                showError(L"Не удалось восстановить документ; журнал сохранён.", r.snapshot ? treeError : r.error);
                return;
            }
            resetDocument(*r.snapshot, {});
            savedRevision_ = std::numeric_limits<std::uint64_t>::max();  // a modified copy (§10.3)
            recoveredName_ = toWide(j.header.name) + L" (восстановлено)";
            writeJournal(true);  // this session's journal holds it before the old one goes
            if (journal_ && journal_->records() > 0) DeleteFileW(j.path.c_str());
            logf("recovered «{}» from {} records", j.header.name, j.records.size());
            logDocument();
            dirty_ = true;
            break;
        }
        case IDNO: DeleteFileW(j.path.c_str()); break;
        default: break;
        }
    }

private:
    // ── Brushes (§13, §14) ──

    [[nodiscard]] static fs::path dataDirectory() {
        std::wstring base(32767, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
        base.resize(length < base.size() ? length : 0);
        return fs::path(base) / L"ChromaStudio";
    }

    [[nodiscard]] static fs::path executableDirectory() {
        std::wstring module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        module.resize(length);
        return fs::path(module).parent_path();
    }

    /// The catalogue: the seven built-in profiles, then the library's (§14.4).
    void loadBrushes() {
        if (!assetsReady_) {
            const fs::path assets = executableDirectory() / L"assets";
            assets_.addDirectory(assets / L"textures");
            assets_.addDirectory(assets / L"materials");
            assets_.addDirectory(dataDirectory() / L"brushes" / L"assets");
            assetsReady_ = true;
        }
        brushes_ = core::builtInBrushes();
        std::string error;
        for (const project::LibraryEntry& e : project::loadLibrary(dataDirectory() / L"brushes", error)) brushes_.push_back(e.profile);
        if (!error.empty()) logf("brush library: {}", error);
        ui_.setCatalog(brushes_);
        logf("brushes: {} in the catalogue ({} from the library)", brushes_.size(), brushes_.size() - core::builtInBrushes().size());
    }

    /// The tip and grain masks a profile refers to, decoded from the local resources (§14.2).
    void resolveMasks(render::BrushSetup& setup, const core::BrushProfile& profile) const {
        setup.tipMask = profile.tip.maskId ? assets_.mask(*profile.tip.maskId, profile.tip.maskChannel) : nullptr;
        setup.grainMask = profile.grain && profile.grain->maskId ? assets_.mask(*profile.grain->maskId, core::MaskChannel::RedAlpha) : nullptr;
        if (profile.tip.maskId && !setup.tipMask) logf("brush «{}»: tip mask not found, the procedural tip is used", profile.name);
        if (profile.ink.blendId != 0 && !warnedBrushBlend_) {
            logf("brush «{}»: its own blend mode is drawn as Normal in this version", profile.name);
            warnedBrushBlend_ = true;
        }
    }

    /// §14.4: preview first (nothing on disk changes), then the import command writes the library.
    void offerImport(const project::ImportResult& result) {
        if (!result.profile) {
            showError(L"Не удалось импортировать кисть.", result.error);
            return;
        }
        const core::BrushProfile& p = *result.profile;
        const bool basic = result.fidelity == "basic";
        const std::wstring question =
            basic ? std::format(L"Восстановлены имя и размер: «{}», {:.1f} px.\n\nИмпортировать базовый профиль пера?", toWide(p.name), p.tip.diameter)
                  : std::format(L"Импортировать кисть «{}» (семейство {}, Ø {:.1f} px)?", toWide(p.name),
                                toWide(std::string(core::builtInBrushes()[static_cast<std::size_t>(p.family)].name)), p.tip.diameter);
        if (MessageBoxW(owner(), question.c_str(), L"Импорт кисти", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        const std::string error = project::saveUserBrush(dataDirectory() / L"brushes", result);
        if (!error.empty()) {
            showError(L"Кисть не сохранена в библиотеке.", error);
            return;
        }
        loadBrushes();
        for (std::size_t i = 0; i < brushes_.size(); ++i) {
            if (brushes_[i].id == p.id) selectBrush(i);
        }
        logf("brush imported: «{}» ({}, {})", p.name, p.id, result.fidelity);
    }

    void importBrushFile() {
        const auto path = chroma::desktop::askImportBrush(owner());
        if (path) offerImport(project::importFromFile(*path, assets_));
    }

    void importBrushFromClipboard() {
        std::wstring text;
        if (OpenClipboard(owner())) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* data = static_cast<const wchar_t*>(GlobalLock(h))) {
                    text = data;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        if (text.empty()) {
            showNotice(L"В буфере обмена нет текста пресета или ссылки");
            return;
        }
        offerImport(project::importFromPayload(toUtf8(text), assets_));  // a link is never fetched (§14)
    }

    // ── Filters (§9) ──

    /// Recomputes the filter preview for the active layer from the pending settings.
    void runFxPreview() {
        fxDirty_ = false;
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles || !fxPending_) return;
        const core::FxSettings fx = core::sanitize(*fxPending_);
        const core::LayerNode* layer = editableLayer();
        if (!layer) {
            tiles->cancelFx();
            fxTarget_ = 0;
            return;
        }
        const Clock::time_point start = Clock::now();
        // The tiles to compute: the layer's own, their neighbours when the filter spreads into
        // transparency, only where the selection reaches.
        const auto& selection = history_.meta().selection;
        std::set<std::pair<int, int>> keys;
        const int gridW = (docWidth_ + render::kTileSize - 1) / render::kTileSize;
        const int gridH = (docHeight_ + render::kTileSize - 1) / render::kTileSize;
        for (const auto& [tx, ty] : presentTiles(layer->id)) {
            const int reach = core::fxSpreads(fx) ? 1 : 0;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    if (tx + dx >= 0 && ty + dy >= 0 && tx + dx < gridW && ty + dy < gridH) keys.insert({tx + dx, ty + dy});
                }
            }
        }
        if (selection) std::erase_if(keys, [&](const auto& k) { return !selection->tile(k.first, k.second); });
        render::FxRequest request;
        request.settings = fx;
        request.source = layer->id;
        request.alphaLock = layer->props.alphaLock;
        request.tiles.assign(keys.begin(), keys.end());
        const bool toNew = fx.kind == core::FxKind::AnimeLines && fx.toNewLayer;
        if (toNew) {
            // §9.6: a separate raster above the source; the source stays.
            if (!fxTree_ || !fxTree_->find(fxTarget_)) {
                fxTree_ = history_.meta().layers;
                fxTarget_ = fxTree_->addRaster("Линии");
            }
            previewTree_ = fxTree_;
            request.target = fxTarget_;
        } else {
            if (fxTree_) previewTree_.reset();
            fxTree_.reset();
            fxTarget_ = layer->id;
            request.target = layer->id;
        }
        tiles->setSelection(selection ? &*selection : nullptr);
        const std::uint64_t source = layer->id;
        tiles->previewFx(request, [this, source](int tx, int ty) { return history_.get(core::TileKey{source, tx, ty}); });
        logf("filter preview «{}»: {} tiles in {:.0f} ms", core::fxName(fx.kind), request.tiles.size(),
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    // ── Raster operations (§10.1) ──

    [[nodiscard]] core::V documentPoint(const PointerSample& s) const {
        const ui::Rect canvas = ui_.layout().canvas;
        return view_.toDocument({s.x - canvas.x, s.y - canvas.y});
    }

    [[nodiscard]] core::TileSource layerSource(std::uint64_t layer) const {
        return [this, layer](int tx, int ty) -> core::TileContent {
            const render::TileRef ref = history_.get(core::TileKey{layer, tx, ty});
            return ref ? ref->sharedData() : nullptr;
        };
    }

    [[nodiscard]] std::vector<std::pair<int, int>> presentTiles(std::uint64_t layer) const {
        std::vector<std::pair<int, int>> keys;
        for (const auto& [key, ref] : tilesOf(layer)) keys.emplace_back(key.tx, key.ty);
        return keys;
    }

    /// The active raster layer when it may be edited; otherwise a notice and nullptr.
    const core::LayerNode* editableLayer(bool vectorAllowed = false) {
        const core::LayerTree& tree = history_.meta().layers;
        const core::LayerNode* layer = activeLayer();
        if (layer && layer->kind == core::NodeKind::Vector && !vectorAllowed) {
            showNotice(L"Векторный слой: для этого действия сначала «Слой → Растеризовать»");
            return nullptr;
        }
        if (!layer || layer->kind == core::NodeKind::Group) {
            showNotice(L"Выбрана группа: это действие работает со слоем");
            return nullptr;
        }
        if (tree.effectivelyLocked(layer->id)) {
            showNotice(std::format(L"Слой «{}» заблокирован", toWide(layer->props.name)));
            return nullptr;
        }
        return layer;
    }

    /// Publishes tile edits of one layer, optionally with new document metadata, as one command.
    bool commitEdits(std::uint64_t layer, const std::vector<core::TileEdit>& edits,
                     std::optional<core::DocumentMeta> meta = std::nullopt) {
        std::vector<render::TileChange> changes;
        appendChanges(changes, layer, edits);
        previewTree_.reset();
        return history_.commit(std::move(changes), std::move(meta));
    }

    void appendChanges(std::vector<render::TileChange>& changes, std::uint64_t layer, const std::vector<core::TileEdit>& edits) {
        for (const core::TileEdit& e : edits) {
            const core::TileKey key{layer, e.tx, e.ty};
            changes.push_back({key, history_.get(key), e.after ? render::TileRenderer::makeVersion(e.after) : nullptr});
        }
    }

    void commitSelection(std::optional<core::SelectionMask> selection) {
        core::DocumentMeta meta = history_.meta();
        meta.selection = std::move(selection);
        history_.commit({}, std::move(meta));
        dirty_ = true;
    }

    /// The composite of the visible tree as a pixel source, composed tile by tile on demand.
    [[nodiscard]] core::PixelSource compositeSource(std::map<std::pair<int, int>, std::vector<core::Pixel>>& cache) const {
        return [this, &cache](int x, int y) -> core::Pixel {
            const auto key = std::pair{x / render::kTileSize, y / render::kTileSize};
            auto it = cache.find(key);
            if (it == cache.end()) {
                std::vector<core::Pixel> pixels(static_cast<std::size_t>(render::kTileSize) * render::kTileSize);
                core::compose(displayTree(), pixels, [&](std::uint64_t layer, std::span<core::Pixel> out) {
                    const render::TileRef ref = history_.get(core::TileKey{layer, key.first, key.second});
                    if (!ref) return false;
                    const auto& h = ref->data().halfs;
                    for (std::size_t i = 0; i < out.size(); ++i) {
                        out[i] = {core::halfToFloat(h[4 * i]), core::halfToFloat(h[4 * i + 1]), core::halfToFloat(h[4 * i + 2]),
                                  core::halfToFloat(h[4 * i + 3])};
                    }
                    return true;
                });
                it = cache.emplace(key, std::move(pixels)).first;
            }
            return it->second[static_cast<std::size_t>(y % render::kTileSize) * render::kTileSize +
                              static_cast<std::size_t>(x % render::kTileSize)];
        };
    }

    [[nodiscard]] static core::SelectOp selectOpFromKeys() {
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt = GetKeyState(VK_MENU) < 0;
        if (shift && alt) return core::SelectOp::Intersect;
        if (shift) return core::SelectOp::Add;
        if (alt) return core::SelectOp::Subtract;
        return core::SelectOp::Replace;
    }

    void beginLasso(const PointerSample& s) {
        gesture_ = Gesture::Lasso;
        owner_ = Owner{s.kind, s.pointerId};
        lassoOp_ = selectOpFromKeys();
        lasso_ = {documentPoint(s)};
    }

    void finishLasso(const PointerSample& s) {
        const core::V end = documentPoint(s);
        lasso_.push_back(end);
        double length = 0.0;
        for (std::size_t i = 1; i < lasso_.size(); ++i) length += std::hypot(lasso_[i].x - lasso_[i - 1].x, lasso_[i].y - lasso_[i - 1].y);
        // A tap is the magic wand; a drawn loop is the lasso.
        const double tapDoc = 3.0 / std::max(view_.zoom, 1e-6);  // 3 DIP
        core::SelectionMask mask;
        if (length < tapDoc) {
            std::map<std::pair<int, int>, std::vector<core::Pixel>> cache;
            mask = core::floodMask(compositeSource(cache), docWidth_, docHeight_, static_cast<int>(std::floor(end.x)),
                                   static_cast<int>(std::floor(end.y)), tolerance_, true);
        } else {
            mask = core::rasterizeLasso(lasso_, docWidth_, docHeight_);
        }
        lasso_.clear();
        commitSelection(core::combine(history_.meta().selection, mask, lassoOp_));
        logf("selection: {}", history_.meta().selection && !history_.meta().selection->empty() ? "set" : "empty");
    }

    void fillAt(const PointerSample& s) {
        const core::LayerNode* layer = editableLayer();
        if (!layer) return;
        const core::V doc = documentPoint(s);
        const int x = static_cast<int>(std::floor(doc.x));
        const int y = static_cast<int>(std::floor(doc.y));
        if (x < 0 || y < 0 || x >= docWidth_ || y >= docHeight_) return;
        const Clock::time_point start = Clock::now();
        std::map<std::pair<int, int>, std::vector<core::Pixel>> cache;
        const bool everywhere = GetKeyState(VK_SHIFT) < 0;  // Shift: not only the connected area
        const core::SelectionMask region =
            core::floodMask(compositeSource(cache), docWidth_, docHeight_, x, y, tolerance_, !everywhere);
        const auto ink = core::premultipliedLinear(color_[0], color_[1], color_[2], alpha_);
        const auto edits = core::fillRegion(layerSource(layer->id), region, history_.meta().selection,
                                            {ink[0], ink[1], ink[2], ink[3]}, layer->props.alphaLock, docWidth_, docHeight_);
        commitEdits(layer->id, edits);
        logf("fill: {} tiles in {:.0f} ms", edits.size(), std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    void beginMove(const PointerSample& s) {
        const core::LayerNode* layer = editableLayer(true);
        if (!layer) return;
        floating_ = {};
        floating_.layer = layer->id;
        const auto& selection = history_.meta().selection;
        const auto present = presentTiles(layer->id);
        if (selection) {
            const core::MaskedSplit split = core::splitMasked(layerSource(layer->id), present, *selection, docWidth_, docHeight_);
            for (const auto& [key, data] : split.base) {
                const render::TileRef same = history_.get(core::TileKey{layer->id, key.first, key.second});
                floating_.base[key] = same && same->sharedData() == data ? same : render::TileRenderer::makeVersion(data);
            }
            for (const auto& [key, data] : split.floating) floating_.floating[key] = render::TileRenderer::makeVersion(data);
        } else {
            for (const auto& [key, ref] : tilesOf(layer->id)) floating_.floating[{key.tx, key.ty}] = ref;
        }
        ++floating_.epoch;
        moveAnchor_ = documentPoint(s);
        gesture_ = Gesture::Move;
        owner_ = Owner{s.kind, s.pointerId};
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) tiles->setFloating(&floating_);
    }

    void updateMove(const PointerSample& s) {
        const core::V doc = documentPoint(s);
        const int dx = static_cast<int>(std::lround(doc.x - moveAnchor_.x));
        const int dy = static_cast<int>(std::lround(doc.y - moveAnchor_.y));
        if (dx == floating_.dx && dy == floating_.dy) return;
        floating_.dx = dx;
        floating_.dy = dy;
        ++floating_.epoch;
    }

    void cancelMove() {
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) tiles->setFloating(nullptr);
        floating_ = {};
    }

    void commitMove() {
        const int dx = floating_.dx;
        const int dy = floating_.dy;
        const std::uint64_t layer = floating_.layer;
        cancelMove();
        if (dx == 0 && dy == 0) return;  // §10.1: a zero move changes nothing
        applyMove(layer, dx, dy);
    }

    void applyMove(std::uint64_t layer, int dx, int dy) {
        if (history_.meta().vectors.contains(layer)) {
            // §10.1: a vector moves by its control points.
            core::DocumentMeta meta = history_.meta();
            for (core::VectorObject& o : meta.vectors[layer]) {
                for (core::Knot& k : o.knots) k.pos = k.pos + core::V{static_cast<double>(dx), static_cast<double>(dy)};
                ++o.version;
            }
            auto changes = rasterizeVectors(layer, meta.vectors[layer]);
            history_.commit(std::move(changes), std::move(meta));
            return;
        }
        const auto& selection = history_.meta().selection;
        const auto edits = core::moveLayer(layerSource(layer), presentTiles(layer), selection, dx, dy, docWidth_, docHeight_);
        core::DocumentMeta meta = history_.meta();
        if (meta.selection) meta.selection = core::translate(*meta.selection, dx, dy, docWidth_, docHeight_);
        commitEdits(layer, edits, std::move(meta));
        logf("move: ({}, {}) px, {} tiles", dx, dy, edits.size());
    }

    void nudge(int dx, int dy) {
        if (const core::LayerNode* layer = editableLayer(true)) applyMove(layer->id, dx, dy);
    }

    // ── Vector lineart (§8) ──

    /// Replays a vector layer's objects into its cached tiles (one GPU stroke per object, in z order)
    /// and returns the changes against the current cache.
    std::vector<render::TileChange> rasterizeVectors(std::uint64_t layer, const core::VectorLayer& objects) {
        std::vector<render::TileChange> changes;
        render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr;
        if (!tiles) return changes;
        std::map<std::pair<int, int>, render::TileRef> cache;
        tiles->setSelection(nullptr);  // the geometry is the object, not the selection
        for (const core::VectorObject& o : objects) {
            const auto ink = core::premultipliedLinear(core::linearToSrgb(o.rgba[0]), core::linearToSrgb(o.rgba[1]),
                                                       core::linearToSrgb(o.rgba[2]), o.rgba[3]);
            render::BrushSetup setup = render::brushSetupFor(o.profile, ink, false);
            setup.layer = layer;
            resolveMasks(setup, o.profile);
            tiles->beginStroke(setup, [&cache](int tx, int ty) -> render::TileRef {
                const auto it = cache.find({tx, ty});
                return it == cache.end() ? nullptr : it->second;
            });
            tiles->applyDabs(core::objectDabs(o));
            for (const render::TileChange& c : tiles->commitStroke()) {
                if (c.after) {
                    cache[{c.key.tx, c.key.ty}] = c.after;
                } else {
                    cache.erase({c.key.tx, c.key.ty});
                }
            }
        }
        std::set<std::pair<int, int>> keys;
        for (const auto& [key, ref] : cache) keys.insert(key);
        for (const auto& [key, ref] : tilesOf(layer)) keys.insert({key.tx, key.ty});
        for (const auto& [tx, ty] : keys) {
            const core::TileKey key{layer, tx, ty};
            const render::TileRef before = history_.get(key);
            const auto it = cache.find({tx, ty});
            const render::TileRef after = it == cache.end() ? nullptr : it->second;
            if (before != after && !(before && after && before->data() == after->data())) changes.push_back({key, before, after});
        }
        return changes;
    }

    /// One command: the vector layer's objects and their replayed pixels.
    void commitVectors(std::uint64_t layer, core::DocumentMeta meta) {
        auto changes = rasterizeVectors(layer, meta.vectors[layer]);
        previewTree_.reset();
        history_.commit(std::move(changes), std::move(meta));
        dirty_ = true;
    }

    void addVectorObject() {
        const core::LayerNode* layer = activeLayer();
        if (!layer || strokeDabs_.empty()) return;
        core::DocumentMeta meta = history_.meta();
        core::VectorObject o;
        o.id = meta.layers.reserveId();
        o.knots = core::fitKnots(strokeDabs_, [&meta] { return meta.layers.reserveId(); });
        o.rgba = {core::srgbToLinear(color_[0]), core::srgbToLinear(color_[1]), core::srgbToLinear(color_[2]), alpha_};
        o.profile = strokeProfile_;
        o.seed = strokeProfile_.grain ? strokeProfile_.grain->seed : 0;
        const std::size_t knots = o.knots.size();
        meta.vectors[layer->id].push_back(std::move(o));
        commitVectors(layer->id, std::move(meta));
        logf("vector object: {} stamps → {} knots", strokeDabs_.size(), knots);
        strokeDabs_.clear();
    }

    [[nodiscard]] const core::VectorObject* findObject(std::uint64_t layer, std::uint64_t id) const {
        const auto it = history_.meta().vectors.find(layer);
        if (it == history_.meta().vectors.end()) return nullptr;
        for (const core::VectorObject& o : it->second) {
            if (o.id == id) return &o;
        }
        return nullptr;
    }

    [[nodiscard]] static core::V knotNormal(const core::Knot& k) {
        core::V t = core::norm(k.out) > 0 ? k.out : core::norm(k.in) > 0 ? k.in * -1.0 : core::V{1, 0};
        const double len = core::norm(t);
        return {-t.y / len, t.x / len};
    }

    void beginVectorEdit(const PointerSample& s) {
        const core::LayerNode* layer = activeLayer();
        if (!layer || layer->kind != core::NodeKind::Vector) {
            showNotice(L"Инструмент V работает на векторном слое (Слой → Новый векторный слой)");
            return;
        }
        if (history_.meta().layers.effectivelyLocked(layer->id)) {
            showNotice(std::format(L"Слой «{}» заблокирован", toWide(layer->props.name)));
            return;
        }
        const core::V doc = documentPoint(s);
        const double tol = 6.0 / std::max(view_.zoom, 1e-6);  // 6 DIP
        const bool doubleClick = s.timeSeconds - lastVectorClick_ <= GetDoubleClickTime() / 1000.0;
        lastVectorClick_ = s.timeSeconds;
        vdrag_ = VectorDrag::None;
        // The selected object's handles first, then its width diamond and knots (§8).
        if (const core::VectorObject* o = vsel_.layer == layer->id ? findObject(layer->id, vsel_.object) : nullptr) {
            if (vsel_.knot >= 0 && static_cast<std::size_t>(vsel_.knot) < o->knots.size()) {
                const core::Knot& k = o->knots[static_cast<std::size_t>(vsel_.knot)];
                if (core::norm(k.out) > 0 && core::norm(doc - (k.pos + k.out)) <= tol) vdrag_ = VectorDrag::HandleOut;
                else if (core::norm(k.in) > 0 && core::norm(doc - (k.pos + k.in)) <= tol) vdrag_ = VectorDrag::HandleIn;
                else if (core::norm(doc - (k.pos + knotNormal(k) * (k.width / 2 + 6.0 / view_.zoom))) <= tol) vdrag_ = VectorDrag::Width;
            }
            if (vdrag_ == VectorDrag::None) {
                for (std::size_t i = 0; i < o->knots.size(); ++i) {
                    if (core::norm(doc - o->knots[i].pos) <= tol) {
                        vsel_.knot = static_cast<int>(i);
                        vdrag_ = VectorDrag::Knot;
                        break;
                    }
                }
            }
            if (vdrag_ != VectorDrag::None) {
                vedit_ = *o;
                vanchor_ = doc;
                gesture_ = Gesture::VectorEdit;
                owner_ = Owner{s.kind, s.pointerId};
                return;
            }
        }
        // An object under the pointer: reverse z order, tolerance width/2 + 6/z (§8).
        const auto it = history_.meta().vectors.find(layer->id);
        if (it != history_.meta().vectors.end()) {
            for (auto o = it->second.rbegin(); o != it->second.rend(); ++o) {
                double distance = 0.0;
                const auto hit = core::nearest(*o, doc, &distance);
                if (!hit || distance > hit->width / 2 + tol) continue;
                if (doubleClick && vsel_.layer == layer->id && vsel_.object == o->id && o->knots.size() > 1 &&
                    hit->t > 0.0 && hit->t < 1.0) {
                    // §8: a double click on the line inserts a knot at the nearest projection.
                    core::DocumentMeta meta = history_.meta();
                    const std::uint64_t knotId = meta.layers.reserveId();
                    for (core::VectorObject& edit : meta.vectors[layer->id]) {
                        if (edit.id == o->id) edit = core::insertKnot(edit, hit->segment, hit->t, knotId);
                    }
                    vsel_.knot = static_cast<int>(hit->segment) + 1;
                    commitVectors(layer->id, std::move(meta));
                    return;
                }
                vsel_ = {layer->id, o->id, -1};
                return;
            }
        }
        vsel_ = {};
    }

    void updateVectorEdit(const PointerSample& s) {
        if (!vedit_ || vsel_.knot < 0) return;
        const core::V doc = documentPoint(s);
        core::Knot& k = vedit_->knots[static_cast<std::size_t>(vsel_.knot)];
        switch (vdrag_) {
        case VectorDrag::Knot: k.pos = doc; break;  // the handles are relative: they come along
        case VectorDrag::HandleOut: core::setHandle(k, true, doc - k.pos); break;
        case VectorDrag::HandleIn: core::setHandle(k, false, doc - k.pos); break;
        case VectorDrag::Width: {
            // §8: twice the distance along the normal; the spacing diameter keeps its ratio.
            const core::V n = knotNormal(k);
            const double width = std::clamp(2.0 * std::abs((doc.x - k.pos.x) * n.x + (doc.y - k.pos.y) * n.y), 0.0, 2048.0);
            k.spacingDiameter = std::clamp(k.width > 0 ? k.spacingDiameter / k.width * width : width, 0.25, 2048.0);
            k.width = width;
            break;
        }
        case VectorDrag::None: break;
        }
    }

    void commitVectorEdit() {
        if (!vedit_) return;
        core::VectorObject edited = std::move(*vedit_);
        vedit_.reset();
        const core::VectorObject* original = findObject(vsel_.layer, edited.id);
        if (!original || *original == edited) return;
        ++edited.version;
        core::DocumentMeta meta = history_.meta();
        for (core::VectorObject& o : meta.vectors[vsel_.layer]) {
            if (o.id == edited.id) o = edited;
        }
        commitVectors(vsel_.layer, std::move(meta));
    }

    /// Delete: the selected knot, or the whole object.
    bool deleteVectorSelection() {
        const core::VectorObject* o = findObject(vsel_.layer, vsel_.object);
        if (!o) return false;
        const std::uint64_t layer = vsel_.layer;
        core::DocumentMeta meta = history_.meta();
        auto& objects = meta.vectors[vsel_.layer];
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            if (it->id != o->id) continue;
            std::optional<core::VectorObject> rest =
                vsel_.knot >= 0 ? core::removeKnot(*it, static_cast<std::size_t>(vsel_.knot)) : std::nullopt;
            if (rest) {
                *it = std::move(*rest);
                vsel_.knot = -1;
            } else {
                objects.erase(it);  // the last point removes the object (§8)
                vsel_ = {};
            }
            break;
        }
        commitVectors(layer, std::move(meta));
        return true;
    }

    /// The selected object, its knots, handles and width diamond, in DIP over the canvas.
    void drawVectorOverlay(ui::UiRenderer& r) {
        if (tool_ != ui::Tool::Vector) return;
        const core::VectorObject* stored = findObject(vsel_.layer, vsel_.object);
        const core::VectorObject* o = vedit_ ? &*vedit_ : stored;
        if (!o) return;
        const ui::Rect canvas = ui_.layout().canvas;
        const auto toDip = [&](const core::V& doc) {
            const core::V v = view_.toView(doc);
            return core::V{canvas.x + v.x, canvas.y + v.y};
        };
        const ui::Color accent = ui_.theme().accent;
        const ui::Color white{1, 1, 1, 1};
        r.pushClip(canvas);
        const auto samples = core::flatten(*o);
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const core::V a = toDip(samples[i - 1].point), b = toDip(samples[i].point);
            r.line(a.x, a.y, b.x, b.y, 1.5, accent);
        }
        for (std::size_t i = 0; i < o->knots.size(); ++i) {
            const core::V p = toDip(o->knots[i].pos);
            const bool selected = static_cast<int>(i) == vsel_.knot;
            r.fill({p.x - 4, p.y - 4, 8, 8}, selected ? accent : white);
            r.stroke({p.x - 4, p.y - 4, 8, 8}, accent, 1.0);
        }
        if (vsel_.knot >= 0 && static_cast<std::size_t>(vsel_.knot) < o->knots.size()) {
            const core::Knot& k = o->knots[static_cast<std::size_t>(vsel_.knot)];
            const core::V p = toDip(k.pos);
            for (const core::V h : {k.in, k.out}) {
                if (core::norm(h) == 0) continue;
                const core::V q = toDip(k.pos + h);
                r.line(p.x, p.y, q.x, q.y, 1.0, accent);
                r.disc(q.x, q.y, 4, white);
                r.ring(q.x, q.y, 4, 1.0, accent);
            }
            // The width diamond on the normal.
            const core::V d = toDip(k.pos + knotNormal(k) * (k.width / 2 + 6.0 / view_.zoom));
            r.line(d.x - 5, d.y, d.x, d.y - 5, 1.5, accent);
            r.line(d.x, d.y - 5, d.x + 5, d.y, 1.5, accent);
            r.line(d.x + 5, d.y, d.x, d.y + 5, 1.5, accent);
            r.line(d.x, d.y + 5, d.x - 5, d.y, 1.5, accent);
        }
        r.popClip();
    }

    void beginCrop(const PointerSample& s) {
        const core::V doc = documentPoint(s);
        cropAnchor_ = {std::clamp(doc.x, 0.0, static_cast<double>(docWidth_)), std::clamp(doc.y, 0.0, static_cast<double>(docHeight_))};
        crop_.reset();
        gesture_ = Gesture::Crop;
        owner_ = Owner{s.kind, s.pointerId};
    }

    void updateCrop(const PointerSample& s) {
        const core::V doc = documentPoint(s);
        const double x = std::clamp(doc.x, 0.0, static_cast<double>(docWidth_));
        const double y = std::clamp(doc.y, 0.0, static_cast<double>(docHeight_));
        // §10.1: integer [left, right) × [top, bottom) inside the original bounds.
        const core::PixelBox box{static_cast<int>(std::lround(std::min(x, cropAnchor_.x))),
                                 static_cast<int>(std::lround(std::min(y, cropAnchor_.y))),
                                 static_cast<int>(std::lround(std::max(x, cropAnchor_.x))),
                                 static_cast<int>(std::lround(std::max(y, cropAnchor_.y)))};
        if (box.empty()) {
            crop_.reset();
        } else {
            crop_ = box;
        }
    }

    void applyCrop() {
        if (!crop_) return;
        const core::PixelBox box = *crop_;
        crop_.reset();
        if (box.x0 == 0 && box.y0 == 0 && box.x1 == docWidth_ && box.y1 == docHeight_) return;
        const int w = box.width();
        const int h = box.height();
        const Clock::time_point start = Clock::now();
        // Every raster layer moves to the new origin; tiles past the new grid go (one command, §10.1).
        std::vector<render::TileChange> changes;
        const int gridW = (w + render::kTileSize - 1) / render::kTileSize;
        const int gridH = (h + render::kTileSize - 1) / render::kTileSize;
        core::DocumentMeta croppedMeta = history_.meta();
        for (const std::uint64_t layer : history_.meta().layers.rasters()) {
            if (croppedMeta.vectors.contains(layer)) continue;  // replayed below at the new size
            const auto present = presentTiles(layer);
            std::vector<core::TileEdit> edits = core::moveLayer(layerSource(layer), present, std::nullopt, -box.x0, -box.y0, w, h);
            if (box.x0 == 0 && box.y0 == 0) {
                // No move: the tiles inside still need the new edge cleared.
                std::vector<core::TileEdit> trimmed;
                core::TileReader reader(layerSource(layer));
                for (const auto& [tx, ty] : present) {
                    if (tx >= gridW || ty >= gridH) continue;
                    const core::TileContent& old = reader.tile(tx, ty);
                    auto data = std::make_shared<core::TileData>(*old);
                    core::canonicalize(*data, core::tileExtent(w, h, tx, ty));
                    if (!(*data == *old)) trimmed.push_back({tx, ty, core::isTransparent(*data) ? nullptr : core::TileContent(data)});
                }
                edits = std::move(trimmed);
            }
            std::set<std::pair<int, int>> touched;
            for (const auto& e : edits) touched.insert({e.tx, e.ty});
            for (const auto& [tx, ty] : present) {
                if ((tx >= gridW || ty >= gridH) && !touched.contains({tx, ty})) edits.push_back({tx, ty, nullptr});
            }
            appendChanges(changes, layer, edits);
        }
        core::DocumentMeta meta = std::move(croppedMeta);
        meta.width = w;
        meta.height = h;
        if (meta.selection) meta.selection = core::translate(*meta.selection, -box.x0, -box.y0, w, h);
        if (!meta.vectors.empty()) {
            // §10.1: vectors move their positions and may extend past the new canvas.
            if (render::TileRenderer* renderer = presenter_ ? presenter_->tiles() : nullptr) renderer->setDocumentSize(w, h);
            for (auto& [layer, objects] : meta.vectors) {
                for (core::VectorObject& o : objects) {
                    for (core::Knot& k : o.knots) k.pos = k.pos - core::V{static_cast<double>(box.x0), static_cast<double>(box.y0)};
                    ++o.version;
                }
                auto replayed = rasterizeVectors(layer, objects);
                changes.insert(changes.end(), replayed.begin(), replayed.end());
            }
        }
        history_.commit(std::move(changes), std::move(meta));
        syncDocumentSize();
        logf("crop: {}x{} at ({}, {}) in {:.0f} ms", w, h, box.x0, box.y0,
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    /// The canvas size follows the history (crop and its undo).
    void syncDocumentSize() {
        const core::DocumentMeta& meta = history_.meta();
        if (meta.width == docWidth_ && meta.height == docHeight_) return;
        docWidth_ = meta.width;
        docHeight_ = meta.height;
        if (render::TileRenderer* renderer = presenter_ ? presenter_->tiles() : nullptr) {
            renderer->setDocumentSize(docWidth_, docHeight_);
        }
        thumbnails_.clear();
        viewFitted_ = false;
        applyGeometry();
    }

    bool editCommand(ui::Command command) {
        dirty_ = true;
        if (gesture_ != Gesture::None) return true;  // §11.2: wait for Up
        const auto& selection = history_.meta().selection;
        switch (command) {
        case ui::Command::SelectAll: commitSelection(core::selectAll(docWidth_, docHeight_)); break;
        case ui::Command::Deselect:
            if (selection) commitSelection(std::nullopt);
            break;
        case ui::Command::InvertSelection:
            if (selection) commitSelection(core::invert(*selection, docWidth_, docHeight_));
            break;
        case ui::Command::Copy:
        case ui::Command::Cut: {
            const core::LayerNode* layer = activeLayer();
            if (!layer || layer->kind == core::NodeKind::Group) {
                showNotice(L"Выбрана группа: копировать можно слой");
                break;
            }
            if (command == ui::Command::Cut && layer->kind == core::NodeKind::Vector) {
                showNotice(L"Векторный слой: вырезать можно после «Слой → Растеризовать»");
                break;
            }
            const core::Image8 image = core::extract(layerSource(layer->id), presentTiles(layer->id), selection, docWidth_, docHeight_);
            if (image.width == 0) {
                showNotice(L"Нечего копировать: выделенная область слоя пуста");
                break;
            }
            const std::string error = chroma::desktop::copyToClipboard(owner(), image);
            if (!error.empty()) {
                // §10.1: when the clipboard refuses, Cut keeps the layer.
                showNotice(std::format(L"Буфер обмена: {}", toWide(error)));
                break;
            }
            logf("copied {}x{} px", image.width, image.height);
            if (command == ui::Command::Cut && editableLayer()) {
                std::vector<core::TileEdit> edits;
                if (selection) {
                    edits = core::clearMasked(layerSource(layer->id), *selection, docWidth_, docHeight_);
                } else {
                    for (const auto& [tx, ty] : presentTiles(layer->id)) edits.push_back({tx, ty, nullptr});
                }
                commitEdits(layer->id, edits);
            }
            break;
        }
        case ui::Command::Paste: {
            std::string error;
            std::optional<core::Image8> image = chroma::desktop::pasteFromClipboard(owner(), error);
            if (!image) {
                showNotice(error.empty() ? L"В буфере обмена нет изображения" : std::format(L"Буфер обмена: {}", toWide(error)));
                break;
            }
            // In the middle of the view; §10.1: a new raster layer above the active one.
            const ui::Rect canvas = ui_.layout().canvas;
            const core::V centre = view_.toDocument({canvas.w / 2, canvas.h / 2});
            image->x = static_cast<int>(std::lround(centre.x - image->width / 2.0));
            image->y = static_cast<int>(std::lround(centre.y - image->height / 2.0));
            core::LayerTree tree = history_.meta().layers;
            const std::uint64_t layer = tree.addRaster("Вставка");
            if (!layer) {
                showNotice(L"Достигнут предел 4096 слоёв");
                break;
            }
            std::vector<render::TileChange> changes;
            appendChanges(changes, layer, core::placeImage(*image, docWidth_, docHeight_));
            commitTree(std::move(tree), std::move(changes));
            logf("pasted {}x{} px at ({}, {})", image->width, image->height, image->x, image->y);
            break;
        }
        default: return false;
        }
        return true;
    }

    /// Lasso path and crop frame over the canvas, in DIP.
    void drawToolOverlays(ui::UiRenderer& r) {
        const ui::Rect canvas = ui_.layout().canvas;
        const auto toDip = [&](const core::V& doc) {
            const core::V v = view_.toView(doc);
            return core::V{canvas.x + v.x, canvas.y + v.y};
        };
        r.pushClip(canvas);
        if (gesture_ == Gesture::Lasso && lasso_.size() > 1) {
            for (std::size_t i = 1; i < lasso_.size(); ++i) {
                const core::V a = toDip(lasso_[i - 1]);
                const core::V b = toDip(lasso_[i]);
                r.line(a.x, a.y, b.x, b.y, 2.5, {0.0f, 0.0f, 0.0f, 0.6f});
                r.line(a.x, a.y, b.x, b.y, 1.0, {1.0f, 1.0f, 1.0f, 1.0f});
            }
        }
        if (crop_) {
            const core::V a = toDip({static_cast<double>(crop_->x0), static_cast<double>(crop_->y0)});
            const core::V b = toDip({static_cast<double>(crop_->x1), static_cast<double>(crop_->y1)});
            const ui::Color shade{0.0f, 0.0f, 0.0f, 0.45f};
            r.fill({canvas.x, canvas.y, canvas.w, a.y - canvas.y}, shade);
            r.fill({canvas.x, b.y, canvas.w, canvas.bottom() - b.y}, shade);
            r.fill({canvas.x, a.y, a.x - canvas.x, b.y - a.y}, shade);
            r.fill({b.x, a.y, canvas.right() - b.x, b.y - a.y}, shade);
            r.stroke({a.x, a.y, b.x - a.x, b.y - a.y}, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0);
        }
        r.popClip();
    }

    void beginNavigate(const PointerSample& s) {
        gesture_ = Gesture::Navigate;
        owner_ = Owner{s.kind, s.pointerId};
        navigateAnchor_ = {s.x, s.y};
        navigatePan_ = view_.pan;
    }

    void endGesture() {
        gesture_ = Gesture::None;
        owner_.reset();
    }

    [[nodiscard]] bool owns(const PointerSample& s) const noexcept {
        return owner_ && owner_->kind == s.kind && owner_->pointerId == s.pointerId;
    }

    // ── Commands ──

    bool undo() {
        if (gesture_ == Gesture::Stroke) return true;  // §11.2: document commands wait for Up
        const auto* changes = history_.undo();
        if (!changes) {
            logf("undo: nothing to undo");
            return true;
        }
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) {
            for (const render::TileChange& c : *changes) {
                if (c.after) tiles->evict(*c.after);
            }
        }
        selectChangedLayer(*changes);
        syncDocumentSize();
        return true;
    }

    /// §10.2: undo and redo update the active layer: the one a stroke changed.
    void selectChangedLayer(const std::vector<render::TileChange>& changes) {
        previewTree_.reset();
        if (changes.empty()) return;
        core::LayerTree tree = history_.meta().layers;
        if (tree.setActive(changes.front().key.layer)) amendTree(std::move(tree));
    }

    bool redo() {
        if (gesture_ == Gesture::Stroke) return true;
        const auto* changes = history_.redo();
        if (!changes) {
            logf("redo: nothing to redo");
            return true;
        }
        if (render::TileRenderer* tiles = presenter_ ? presenter_->tiles() : nullptr) {
            for (const render::TileChange& c : *changes) {
                if (c.before) tiles->evict(*c.before);
            }
        }
        selectChangedLayer(*changes);
        syncDocumentSize();
        return true;
    }

    bool fitView() {
        if (gesture_ != Gesture::None) return true;
        const ui::Rect canvas = ui_.layout().canvas;
        view_.fit({canvas.w, canvas.h}, {static_cast<double>(docWidth_), static_cast<double>(docHeight_)});
        return true;
    }

    bool actualPixels() {
        if (gesture_ != Gesture::None) return true;
        const ui::Rect canvas = ui_.layout().canvas;
        view_.actualPixels({canvas.w, canvas.h}, {static_cast<double>(docWidth_), static_cast<double>(docHeight_)},
                           metrics_.dpi);
        return true;
    }

    // ── Documents (spec §10.1, §10.3) ──

    /// Document commands run between gestures only (§11.2).
    template <class Fn>
    bool runCommand(Fn&& fn) {
        if (gesture_ != Gesture::None) return true;
        fn();
        dirty_ = true;
        return true;
    }

    [[nodiscard]] bool dirty() const noexcept { return history_.revision() != savedRevision_; }

    [[nodiscard]] std::wstring documentName() const {
        if (path_.empty()) return recoveredName_.empty() ? std::wstring(L"Без имени") : recoveredName_;
        return path_.filename().wstring();
    }

    [[nodiscard]] HWND owner() const noexcept { return window_ ? window_->handle() : nullptr; }

    void showError(const std::wstring& what, const std::string& reason) const {
        logf("{}: {}", toUtf8(what), reason);
        const std::wstring text = what + L"\n\n" + toWide(reason);
        MessageBoxW(owner(), text.c_str(), L"ChromaStudio", MB_OK | MB_ICONERROR);
    }

    /// True when the current document may be closed or replaced: saved, discarded, or unchanged.
    bool confirmDiscard() {
        if (!dirty()) return true;
        const std::wstring question = std::format(L"Сохранить изменения в «{}»?", documentName());
        switch (MessageBoxW(owner(), question.c_str(), L"ChromaStudio", MB_YESNOCANCEL | MB_ICONWARNING)) {
        case IDYES: return save();
        case IDNO: return true;
        default: return false;
        }
    }

    // Replaces the document state; the history starts empty and the result is clean. The snapshot's
    // tree has been validated (project_io, or layerTree() by the caller).
    void resetDocument(const project::ProjectSnapshot& snapshot, fs::path path) {
        if (fxPending_) fxCancel();
        crop_.reset();
        lasso_.clear();
        floating_ = {};
        std::string error;
        std::optional<core::LayerTree> tree = project::layerTree(snapshot, error);
        if (!tree) tree = core::LayerTree::single();
        docWidth_ = snapshot.width;
        docHeight_ = snapshot.height;
        docDpi_ = snapshot.dpi;
        std::vector<std::pair<core::TileKey, render::TileRef>> tiles;
        tiles.reserve(snapshot.tiles.size());
        for (const project::TileEntry& t : snapshot.tiles) {
            tiles.emplace_back(core::TileKey{t.layer, t.tx, t.ty}, render::TileRenderer::makeVersion(t.data));
        }
        core::DocumentMeta meta;
        meta.layers = std::move(*tree);
        meta.selection = snapshot.selection;
        meta.width = snapshot.width;
        meta.height = snapshot.height;
        meta.vectors = snapshot.vectors;
        history_.replaceAll(std::move(tiles), meta);
        if (!meta.vectors.empty()) {
            // Vector pixels are replayed from the objects; they join the base state of the history.
            if (render::TileRenderer* renderer = presenter_ ? presenter_->tiles() : nullptr) {
                renderer->setDocumentSize(snapshot.width, snapshot.height);
            }
            std::vector<std::pair<core::TileKey, render::TileRef>> all;
            history_.forEachTile([&](const core::TileKey& key, const render::TileRef& ref) { all.emplace_back(key, ref); });
            for (const auto& [layer, objects] : meta.vectors) {
                for (const render::TileChange& c : rasterizeVectors(layer, objects)) {
                    if (c.after) all.emplace_back(c.key, c.after);
                }
            }
            history_.replaceAll(std::move(all), std::move(meta));
        }
        previewTree_.reset();
        thumbnails_.clear();
        savedRevision_ = history_.revision();
        path_ = std::move(path);
        identity_.reset();
        if (render::TileRenderer* renderer = presenter_ ? presenter_->tiles() : nullptr) {
            renderer->setDocumentSize(docWidth_, docHeight_);
        }
        viewFitted_ = false;
        applyGeometry();
        recoveredName_.clear();
        restartJournal(path_);
    }

    [[nodiscard]] project::ProjectSnapshot snapshot() const {
        project::ProjectSnapshot s;
        s.width = docWidth_;
        s.height = docHeight_;
        s.dpi = docDpi_;
        project::setLayerTree(s, history_.meta().layers);
        s.selection = history_.meta().selection;
        s.vectors = history_.meta().vectors;
        history_.forEachTile([&](const core::TileKey& key, const render::TileRef& ref) {
            if (s.vectors.contains(key.layer)) return;  // vector pixels are replayed, not stored (§10.3)
            s.tiles.push_back({key.layer, key.tx, key.ty, ref->sharedData()});
        });
        return s;
    }

    void logDocument() const {
        logf("document: {} — {}x{} px, {} layers, {} tiles stored{}", toUtf8(documentName()), docWidth_, docHeight_,
             history_.meta().layers.size(), history_.tileCount(), dirty() ? ", unsaved changes" : "");
    }

    void newDocument() {
        if (!confirmDiscard()) return;
        resetDocument(project::emptyProject(options_.documentWidth, options_.documentHeight), {});
        logDocument();
    }

    void openWithDialog() {
        if (!confirmDiscard()) return;
        if (const auto path = chroma::desktop::askOpenProject(owner(), path_)) openDocument(*path);
    }

    void openDocument(const fs::path& path) {
        const Clock::time_point start = Clock::now();
        project::LoadResult loaded = project::loadProject(path);
        if (!loaded.snapshot) {
            showError(std::format(L"Не удалось открыть «{}».", path.filename().wstring()), loaded.error);
            return;
        }
        // Validated in full before the current document is replaced (spec §10.3).
        std::string treeError;
        if (!project::layerTree(*loaded.snapshot, treeError)) {
            showError(std::format(L"Не удалось открыть «{}».", path.filename().wstring()), treeError);
            return;
        }
        resetDocument(*loaded.snapshot, path);
        identity_ = loaded.identity;
        logf("opened {} in {:.0f} ms", toUtf8(path.wstring()),
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        logDocument();
    }

    bool save() {
        if (path_.empty()) return saveAs();
        // §10.3: a file changed by another process since we read or wrote it gets a copy instead.
        if (identity_) {
            const auto now = project::identifyFile(path_);
            if (now && *now != *identity_) {
                const std::wstring question = std::format(
                    L"Файл «{}» изменён другой программой после открытия.\n\nДа — сохранить копию под другим именем.\n"
                    L"Нет — перезаписать файл.",
                    documentName());
                switch (MessageBoxW(owner(), question.c_str(), L"ChromaStudio", MB_YESNOCANCEL | MB_ICONWARNING)) {
                case IDYES: return saveAs();
                case IDNO: break;
                default: return false;
                }
            }
        }
        return saveTo(path_);
    }

    bool saveAs() {
        fs::path suggested = path_;
        if (suggested.empty()) suggested = L"Без имени.rawproj";
        const auto path = chroma::desktop::askSaveProject(owner(), suggested);
        return path && saveTo(*path);
    }

    bool saveTo(const fs::path& path) {
        const Clock::time_point start = Clock::now();
        const std::uint64_t revision = history_.revision();  // the immutable revision being written
        project::ProjectSnapshot saved = snapshot();
        saved.thumbnailPng = thumbnail();
        const project::SaveResult result = project::saveProject(path, saved);
        if (!result.ok) {
            showError(std::format(L"Не удалось сохранить «{}».", path.filename().wstring()), result.error);
            return false;
        }
        // §10.3: clean only after the file is confirmed, and only if this is still that revision.
        if (history_.revision() == revision) savedRevision_ = revision;
        path_ = path;
        identity_ = result.identity;
        recoveredName_.clear();
        restartJournal(path_);  // the saved file is the new base; the old generation goes
        logf("saved {} ({} tiles) in {:.0f} ms", toUtf8(path.wstring()), history_.tileCount(),
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        return true;
    }

    /// §10.3: a PNG or JPEG becomes a new raster layer named after the file, centred in the document.
    void importPicture(const fs::path& path) {
        const auto bytes = project::readFileBytes(path);
        std::string error;
        project::PictureInfo info;
        std::optional<project::ImageRgba> picture;
        if (!bytes) {
            error = "файл не читается";
        } else {
            picture = project::decodePicture(*bytes, error, info);
        }
        if (!picture) {
            showError(std::format(L"Не удалось импортировать «{}».", path.filename().wstring()), error);
            return;
        }
        core::Image8 image;
        image.width = picture->width;
        image.height = picture->height;
        image.rgba = std::move(picture->rgba);
        image.x = (docWidth_ - image.width) / 2;
        image.y = (docHeight_ - image.height) / 2;
        core::LayerTree tree = history_.meta().layers;
        const std::uint64_t layer = tree.addRaster(toUtf8(path.stem().wstring()));
        if (!layer) {
            showNotice(L"Достигнут предел 4096 слоёв");
            return;
        }
        std::vector<render::TileChange> changes;
        appendChanges(changes, layer, core::placeImage(image, docWidth_, docHeight_));
        commitTree(std::move(tree), std::move(changes));
        logf("imported {} — {}x{} px, EXIF orientation {}{}", toUtf8(path.wstring()), image.width, image.height, info.orientation,
             info.profileConverted ? ", ICC → sRGB" : info.profileIgnored ? ", ICC not converted (taken as sRGB)" : "");
        if (info.profileIgnored) showNotice(L"Встроенный цветовой профиль не преобразован: цвета взяты как sRGB");
    }

    void onFilesDropped(const std::vector<std::wstring>& paths) override {
        for (const std::wstring& item : paths) {
            const fs::path path(item);
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (ext == L".rawproj") {
                runCommand([this, path] {
                    if (confirmDiscard()) openDocument(path);
                });
                return;  // one document at a time
            }
            if (ext == L".png" || ext == L".jpg" || ext == L".jpeg") {
                runCommand([this, path] { importPicture(path); });
            } else if (ext == L".json" || ext == L".ipb") {
                runCommand([this, path] { offerImport(project::importFromFile(path, assets_)); });
            } else {
                showNotice(std::format(L"«{}»: перетаскивайте проекты, PNG, JPEG или кисти", path.filename().wstring()));
            }
        }
    }

    /// One tile of the flattened visible tree (§7), canonical, nullptr when transparent.
    [[nodiscard]] std::shared_ptr<const core::TileData> composedTile(const core::LayerTree& tree, int tx, int ty) const {
        std::vector<core::Pixel> pixels(static_cast<std::size_t>(render::kTileSize) * render::kTileSize);
        bool any = false;
        core::compose(tree, pixels, [&](std::uint64_t layer, std::span<core::Pixel> out) {
            const render::TileRef ref = history_.get(core::TileKey{layer, tx, ty});
            if (!ref) return false;
            const auto& h = ref->data().halfs;
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = {core::halfToFloat(h[4 * i]), core::halfToFloat(h[4 * i + 1]), core::halfToFloat(h[4 * i + 2]),
                          core::halfToFloat(h[4 * i + 3])};
            }
            any = true;
            return true;
        });
        if (!any) return nullptr;
        auto data = std::make_shared<core::TileData>();
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            data->halfs[4 * i] = core::floatToHalf(pixels[i].r);
            data->halfs[4 * i + 1] = core::floatToHalf(pixels[i].g);
            data->halfs[4 * i + 2] = core::floatToHalf(pixels[i].b);
            data->halfs[4 * i + 3] = core::floatToHalf(pixels[i].a);
        }
        core::canonicalize(*data, core::tileExtent(docWidth_, docHeight_, tx, ty));
        if (core::isTransparent(*data)) return nullptr;
        return data;
    }

    /// thumbnail.png (§10.3): the flattened image at most 512 px, box-filtered in linear PMA, straight
    /// sRGB. Optional, so very large documents go without one rather than slow every save.
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> thumbnail() const {
        const int tilesX = (docWidth_ + render::kTileSize - 1) / render::kTileSize;
        const int tilesY = (docHeight_ + render::kTileSize - 1) / render::kTileSize;
        if (static_cast<std::int64_t>(tilesX) * tilesY > 4096) return nullptr;
        const double scale = std::min(1.0, 512.0 / std::max(docWidth_, docHeight_));
        const int tw = std::max(1, static_cast<int>(std::lround(docWidth_ * scale)));
        const int th = std::max(1, static_cast<int>(std::lround(docHeight_ * scale)));
        const auto cell = [&](int v, int size) { return std::min(size - 1, static_cast<int>(v * scale)); };
        std::vector<int> countX(static_cast<std::size_t>(tw)), countY(static_cast<std::size_t>(th));
        for (int x = 0; x < docWidth_; ++x) ++countX[static_cast<std::size_t>(cell(x, tw))];
        for (int y = 0; y < docHeight_; ++y) ++countY[static_cast<std::size_t>(cell(y, th))];
        std::vector<double> sum(static_cast<std::size_t>(tw) * th * 4, 0.0);
        const core::LayerTree& tree = history_.meta().layers;
        for (int ty = 0; ty < tilesY; ++ty) {
            for (int tx = 0; tx < tilesX; ++tx) {
                const auto tile = composedTile(tree, tx, ty);
                if (!tile) continue;
                const core::TileExtent e = core::tileExtent(docWidth_, docHeight_, tx, ty);
                for (int y = 0; y < e.validHeight; ++y) {
                    const std::size_t row = static_cast<std::size_t>(cell(ty * render::kTileSize + y, th)) * tw;
                    for (int x = 0; x < e.validWidth; ++x) {
                        double* out = &sum[(row + static_cast<std::size_t>(cell(tx * render::kTileSize + x, tw))) * 4];
                        const std::uint16_t* p = &tile->halfs[(static_cast<std::size_t>(y) * render::kTileSize + x) * 4];
                        for (int c = 0; c < 4; ++c) out[c] += core::halfToFloat(p[c]);
                    }
                }
            }
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(tw) * th * 4, 0);
        for (int v = 0; v < th; ++v) {
            for (int u = 0; u < tw; ++u) {
                const std::size_t i = static_cast<std::size_t>(v) * tw + u;
                const double n = static_cast<double>(countX[static_cast<std::size_t>(u)]) * countY[static_cast<std::size_t>(v)];
                const double a = std::clamp(sum[i * 4 + 3] / n, 0.0, 1.0);
                const auto alpha8 = static_cast<std::uint8_t>(std::lround(a * 255.0));
                if (alpha8 == 0) continue;  // RGB exactly zero where alpha is zero
                for (int c = 0; c < 3; ++c) {
                    const double straight = std::clamp(sum[i * 4 + static_cast<std::size_t>(c)] / n / a, 0.0, 1.0);
                    rgba[i * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::lround(core::linearToSrgb(straight) * 255.0));
                }
                rgba[i * 4 + 3] = alpha8;
            }
        }
        auto png = project::encodePng(tw, th, rgba);
        if (png.empty()) return nullptr;
        return std::make_shared<const std::vector<std::byte>>(std::move(png));
    }

    void exportWithDialog() {
        fs::path suggested = path_.empty() ? fs::path(L"Без имени.png") : path_;
        suggested.replace_extension(L".png");
        const auto path = chroma::desktop::askExportPng(owner(), suggested);
        if (!path) return;
        std::wstring ext = path->extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        const bool jpeg = ext == L".jpg" || ext == L".jpeg";
        const Clock::time_point start = Clock::now();
        // The flattened image of the visible tree (§7); PNG keeps alpha without the paper, JPEG is
        // composed over white in linear before encoding (§10.3).
        const core::LayerTree tree = history_.meta().layers;
        const project::TileSource source = [this, &tree](int tx, int ty) { return composedTile(tree, tx, ty); };
        const std::string error = jpeg ? project::exportJpeg(*path, docWidth_, docHeight_, source, {1.0f, 1.0f, 1.0f})
                                       : project::exportPng(*path, docWidth_, docHeight_, source);
        if (!error.empty()) {
            showError(std::format(L"Не удалось экспортировать «{}».", path->filename().wstring()), error);
            return;
        }
        logf("exported {} in {:.0f} ms", toUtf8(path->wstring()),
             std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    // ── Presentation ──

    void createPresenter() {
        thumbnails_.clear();
        presenter_.reset();
        std::string failure = "disabled by --cpu";
        if (!options_.forceCpu) presenter_ = GlPresenter::create(window_->handle(), docWidth_, docHeight_, failure);
        if (!presenter_) {
            logf("GPU presentation unavailable: {}; using CPU/DIB without the interface and painting", failure);
            presenter_ = std::make_unique<GdiPresenter>(window_->handle());
        }
        logf("presenter: {}", presenter_->label());
        if (ui::BrushPreview* preview = presenter_->brushPreview()) {
            preview->setMaskResolver([this](render::BrushSetup& setup, const core::BrushProfile& p) { resolveMasks(setup, p); });
        }
    }

    // Committed tiles live in RAM; GPU copies are a cache. A lost device costs only the unfinished
    // stroke: the new renderer uploads every version again on first use (spec §2).
    void recoverPresenter() {
        ++deviceLosses_;
        logf("presentation device lost ({} of {}); the document is kept in RAM, an unfinished stroke is dropped",
             deviceLosses_, kMaxDeviceLosses);
        if (deviceLosses_ >= kMaxDeviceLosses) options_.forceCpu = true;
        builder_.reset();
        endGesture();
        createPresenter();
        applyGeometry();
    }

    [[nodiscard]] RectPx canvasPx() const { return toPhysical(ui_.layout().canvas, metrics_.scale()); }

    [[nodiscard]] render::DocumentFrame documentFrame() const {
        const RectPx canvas = canvasPx();
        const ui::Rect canvasDip = ui_.layout().canvas;
        render::DocumentFrame frame;
        frame.clientWidth = metrics_.widthPx;
        frame.clientHeight = metrics_.heightPx;
        frame.viewportX0 = canvas.x0;
        frame.viewportY0 = canvas.y0;
        frame.viewportX1 = canvas.x1;
        frame.viewportY1 = canvas.y1;
        const double scale = metrics_.scale();
        const core::V origin = view_.toView({0.0, 0.0});
        frame.originX = (canvasDip.x + origin.x) * scale;
        frame.originY = (canvasDip.y + origin.y) * scale;
        frame.pixelsPerDocPx = view_.zoom * scale;
        return frame;
    }

    [[nodiscard]] RectPx paperRect(const render::DocumentFrame& frame) const {
        const double k = frame.pixelsPerDocPx;
        return {static_cast<int>(std::lround(frame.originX)), static_cast<int>(std::lround(frame.originY)),
                static_cast<int>(std::lround(frame.originX + docWidth_ * k)),
                static_cast<int>(std::lround(frame.originY + docHeight_ * k))};
    }

    void applyGeometry() {
        dirty_ = true;
        if (metrics_.minimized || metrics_.widthPx <= 0 || metrics_.heightPx <= 0) return;
        const ui::Rect canvas = ui_.layout().canvas;
        if (!viewFitted_ && canvas.w > 0 && canvas.h > 0) {
            fitView();
            viewFitted_ = true;
        }
        if (presenter_) presenter_->resize(metrics_.widthPx, metrics_.heightPx);
    }

    void logGeometry() const {
        const RectPx canvas = canvasPx();
        logf("window: client {}x{} px = {:.2f}x{:.2f} DIP at {} dpi; canvas {}x{} px at ({}, {}); dock {}; zoom {:.1f}%",
             metrics_.widthPx, metrics_.heightPx, metrics_.widthDip, metrics_.heightDip, metrics_.dpi, canvas.width(),
             canvas.height(), canvas.x0, canvas.y0, ui_.layout().dockVisible ? "shown" : "hidden",
             view_.zoom * metrics_.scale() * 100.0);
    }

    void traceSample(const PointerSample& s) const {
        logf("in #{} {} {} {} t={:.6f} px=({:.3f}, {:.3f}) dip=({:.3f}, {:.3f}) p={:.4f} tilt=({:.4f}, {:.4f}) "
             "twist={:.4f} buttons=0x{:X} axes=0x{:X} flags=0x{:X} frame={} gen={}{}",
             s.sequence, kindName(s.kind), s.pointerId, phaseName(s.phase), s.timeSeconds, s.xPx, s.yPx, s.x, s.y,
             s.pressure, s.tiltX, s.tiltY, s.twist, s.buttons, s.validAxes, s.flags, s.frameId, s.windowGeneration,
             s.phase == PointerPhase::Cancel ? std::format(" reason={}", reasonName(s.cancelReason)) : "");
    }

    ShellOptions options_;
    PlatformWindow* window_ = nullptr;
    std::unique_ptr<Presenter> presenter_;
    int deviceLosses_ = 0;
    bool warnedNoGpu_ = false;
    WindowMetrics metrics_{};
    bool dirty_ = true;
    std::wstring lastTitle_;

    ui::EditorUi ui_;
    render::TileHistory history_;
    render::LayerStack stack_;
    std::optional<core::LayerTree> previewTree_;  // a layer opacity being dragged
    double tolerance_ = 0.1;                       // fill and wand (§10.1)
    std::vector<core::V> lasso_;
    core::SelectOp lassoOp_ = core::SelectOp::Replace;
    render::FloatingLayer floating_;               // the move preview
    core::V moveAnchor_;
    std::optional<core::PixelBox> crop_;           // the frame waiting for Enter
    std::optional<core::FxSettings> fxPending_;    // the open filter dialog's settings
    bool fxDirty_ = false;
    std::optional<core::LayerTree> fxTree_;        // "to a new layer": the tree with that layer
    std::uint64_t fxTarget_ = 0;
    // Brushes.
    mutable project::AssetStore assets_;  // caches decoded masks
    bool assetsReady_ = false;
    mutable bool warnedBrushBlend_ = false;
    std::vector<core::BrushProfile> brushes_;
    // Recovery journal.
    std::string sessionId_ = project::newSessionId();
    std::unique_ptr<project::Journal> journal_;
    project::JournalState journalState_;
    std::string journalBase_;  // UTF-8 path of the file the records build on, empty for a new document
    std::string journalBaseSha_;
    std::uint64_t journalRevision_ = 0;
    Clock::time_point journalDue_ = Clock::now() + kJournalPeriod;
    int journalGeneration_ = 0;
    bool journalWarned_ = false;
    std::wstring recoveredName_;
    // Vector lineart.
    bool strokeVector_ = false;
    core::BrushProfile strokeProfile_;
    std::vector<core::Dab> strokeDabs_;
    struct VectorSelection {
        std::uint64_t layer = 0;
        std::uint64_t object = 0;
        int knot = -1;
    } vsel_;
    enum class VectorDrag : std::uint8_t { None, Knot, HandleIn, HandleOut, Width } vdrag_ = VectorDrag::None;
    std::optional<core::VectorObject> vedit_;
    core::V vanchor_;
    double lastVectorClick_ = -10.0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> duplicated_;
    core::V cropAnchor_;
    std::wstring notice_;
    core::ViewTransform view_;
    bool viewFitted_ = false;

    // The document (spec §10.3); the layer tree lives in the history.
    int docWidth_ = 1;
    int docHeight_ = 1;
    int docDpi_ = 96;
    fs::path path_;
    std::optional<project::FileIdentity> identity_;
    std::uint64_t savedRevision_ = 0;

    // Tool and quick controls.
    ui::Tool tool_ = ui::Tool::Brush;
    std::size_t brushIndex_ = 0;
    double diameter_ = 12.0;
    double opacity_ = 1.0;
    double stabilizerMs_ = 10.0;
    core::Rgb color_{0.0, 0.0, 0.0};  // encoded sRGB
    double alpha_ = 1.0;

    Gesture gesture_ = Gesture::None;
    std::optional<Owner> owner_;
    std::optional<core::StrokeBuilder> builder_;
    std::vector<core::Dab> finalDabs_;
    std::vector<core::Dab> tailDabs_;
    std::size_t dabCount_ = 0;
    bool strokeErase_ = false;
    Clock::time_point strokeStarted_{};
    core::V navigateAnchor_;
    core::V navigatePan_;
    bool spaceHeld_ = false;

    std::optional<PointerSample> cursor_;
    std::uint32_t loggedDpi_ = 0;

    std::unordered_map<std::uint64_t, Thumbnail> thumbnails_;
};

// ═════════════════════════════════════════════════════════════════════════════
// Low-latency message loop
// ═════════════════════════════════════════════════════════════════════════════

int runMessageLoop(PlatformWindow& window, EditorShell& shell) {
    // Bound on one drain pass, so a flood of messages cannot starve frames.
    constexpr auto kDrainBudget = std::chrono::milliseconds(8);
    for (;;) {
        // 1. Drain everything queued. Pen history is copied inside the handlers, so the frame that
        //    follows sees all input that reached the queue before it started.
        const Clock::time_point drainStart = Clock::now();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
            TranslateMessage(&message);
            DispatchMessageW(&message);
            window.rethrowPendingFailure();
            if (Clock::now() - drainStart > kDrainBudget) break;
        }
        // 2. One frame from the newest state, or sleep until input arrives. Nothing renders while idle.
        shell.updateTitle();
        shell.maintainJournal();
        if (shell.needsFrame()) {
            shell.renderFrame();
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, shell.idleTimeoutMs(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}

// --canvas=WxH, 1..32768 per axis (spec §2).
bool parseCanvasSize(std::wstring_view value, int& width, int& height) {
    const std::wstring text(value);
    wchar_t* end = nullptr;
    const long w = std::wcstol(text.c_str(), &end, 10);
    if (!end || (*end != L'x' && *end != L'X')) return false;
    const long h = std::wcstol(end + 1, &end, 10);
    if (!end || *end != L'\0' || w < 1 || w > 32768 || h < 1 || h > 32768) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

ShellOptions parseArguments() {
    ShellOptions options;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return options;
    for (int i = 1; i < count; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--cpu") {
            options.forceCpu = true;
        } else if (arg == L"--trace-input") {
            options.traceInput = true;
        } else if (arg.starts_with(L"--canvas=")) {
            if (!parseCanvasSize(arg.substr(9), options.documentWidth, options.documentHeight)) {
                logf("--canvas must be WxH with 1..32768 per axis; using {}x{}", options.documentWidth,
                     options.documentHeight);
            }
        } else if (!arg.starts_with(L"--") && options.openAtStart.empty()) {
            options.openAtStart = fs::path(arg);  // a project passed by Explorer or on the command line
        } else {
            logf("unknown argument ignored: {}", toUtf8(arg));
        }
    }
    LocalFree(argv);
    return options;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int showCommand) {
    // Spec §2: Per-Monitor V2 before any HWND. The manifest declares it; this is the fallback.
    const bool perMonitorV2 = PlatformWindow::enablePerMonitorDpiAwareness();
    // File dialogs and WIC need COM on the UI thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    openLog();
    logf("ChromaStudio Engine {}", CHROMA_VERSION);
    logf("DPI awareness: {}", perMonitorV2 ? "Per-Monitor V2" : "NOT Per-Monitor V2, coordinates may be virtualised");
    if (FAILED(com)) logf("COM initialisation failed (0x{:08X}): file dialogs and PNG export are unavailable",
                          static_cast<std::uint32_t>(com));
    logAssetInventory();

    ShellOptions options = parseArguments();
    int exitCode = 1;
    try {
        EditorShell shell(std::move(options));
        PlatformWindow window(instance, WindowDesc{}, shell);
        shell.attach(window);
        window.show(showCommand);
        shell.offerRecovery();
        exitCode = runMessageLoop(window, shell);
        logf("exit code {}", exitCode);
    } catch (const std::exception& error) {
        logf("fatal: {}", error.what());
        MessageBoxW(nullptr, toWide(error.what()).c_str(), L"ChromaStudio", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return exitCode;
}

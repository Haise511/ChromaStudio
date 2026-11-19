#pragma once
// ChromaStudio Engine · render_gl · dry strokes and the layer compositor on 128-px tiles (spec §2, §4, §7,
// §10.2).
//
// A committed tile is a TileVersion: its canonical binary16 content lives in RAM (spec §2: a commit is
// durable once the canonical tiles are in RAM), and a GPU texture is only a cache of it. Textures are
// uploaded on first use and can be evicted at any time; after a device loss a new renderer simply
// uploads again, so neither the document nor its history is lost.
//
// A stroke is one transaction on one raster layer:
//   max mode    every stamp writes its §4.1 weight into a per-tile R16F scratch with GL_MAX blending,
//               so overlapping stamps of one gesture never raise opacity; commit resolves
//               before ∘ (ink · weight) into a new tile.
//   build mode  the first write copies `before` into a working tile; each stamp writes its weight
//               into a temporary R16F and is composited source-over (or erased) into the working tile.
//   Alpha Lock  keeps the alpha of `before` (§7): paint changes only the colour inside it.
//   wet         watercolor, smudge and blur (§5) run stamp by stamp: each stamp assembles its region
//               of the layer with a halo from the working tiles, writes the §4.1 mask on the same grid,
//               runs §5.1 (or the §9.2 blur and its mix) and writes the stamp's box back, so the next
//               stamp sees the result. The reservoir H′ is updated once per stamp from a 1 × 1 read-back.
//               Wet strokes have no provisional tail.
// The provisional tail of the stroke is drawn into per-tile preview copies rebuilt on every update,
// so the confirmed part is never painted over itself. `before` versions are never written: cancel
// drops the stroke state, commit reads the result back, canonicalises it and hands (before, after)
// pairs to the history. A result equal to `before` collapses to it; a fully transparent one becomes
// "no tile". The invisible remainder of edge tiles stays zero: stamps are scissored to the document.
//
// Display composes the layer tree per visible tile with the §7.1 program into an RGBA16F tile (the
// active stroke enters as its resolved state), then draws that tile over the paper. Composed tiles
// are cached by a signature of the tree's render state and the versions they were made from, so a
// stroke recomposes only the tiles it touches.

#include "core/Document.hpp"
#include "core/Fx.hpp"
#include "core/LayerTree.hpp"
#include "core/MaskImage.hpp"
#include "core/StrokeBuilder.hpp"
#include "core/TileData.hpp"
#include "core/TileHistory.hpp"
#include "render_gl/GlContext.hpp"
#include "render_gl/GlProgram.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace chroma::core {
struct BrushProfile;
}

namespace chroma::render {

inline constexpr int kTileSize = core::kTileSize;            // spec §2
inline constexpr std::size_t kTileBytes = core::kTileBytes;  // 128 × 128 × RGBA × binary16

class TexturePool;

/// One immutable tile version: canonical content in RAM plus an optional GPU copy.
class TileVersion {
public:
    explicit TileVersion(std::shared_ptr<const core::TileData> data);
    ~TileVersion();
    TileVersion(const TileVersion&) = delete;
    TileVersion& operator=(const TileVersion&) = delete;

    [[nodiscard]] const core::TileData& data() const noexcept { return *data_; }
    [[nodiscard]] const std::shared_ptr<const core::TileData>& sharedData() const noexcept { return data_; }
    /// Unique for the life of the process: identifies the content in cache signatures.
    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }

private:
    friend class TileRenderer;
    std::shared_ptr<const core::TileData> data_;
    std::uint64_t serial_;
    mutable std::weak_ptr<TexturePool> pool_;  // the renderer that holds the GPU copy
    mutable GLuint texture_ = 0;
};

using TileRef = std::shared_ptr<const TileVersion>;
using TileHistory = core::TileHistory<TileRef, core::DocumentMeta>;  // tree, selection, size share the transactions
using TileChange = TileHistory::Change;
using TileLookup = std::function<TileRef(int tx, int ty)>;

enum class StrokeKind : std::uint8_t { Dry, Watercolor, Smudge, Blur };

/// Per-stroke constants of the §4.1 program, the dry composite and the §5 wet pass.
struct BrushSetup {
    std::uint64_t layer = 1;  // the raster layer the stroke writes
    StrokeKind stroke = StrokeKind::Dry;  // dry stamps or a §5 wet tool
    bool erase = false;
    bool alphaLock = false;  // §7: keep the alpha of the layer
    bool maxMode = true;     // ink.accumulate == max
    int kind = 0;            // 0 tip, 1 airbrush, 2 texture, 3 screentone
    float hardness = 1.0f;
    std::array<float, 4> inkPma{0.0f, 0.0f, 0.0f, 1.0f};  // linear premultiplied; alpha = colour alpha
    float grainPx = 1.0f;
    float grainAmount = 0.0f;
    std::uint32_t seed = 0;
    float periodPx = 8.0f;
    float density = 0.0f;
    float toneAngle = 0.0f;  // radians
    // §5: wet parameters (watercolor from the profile; smudge b = 1, d = 0, e = 0.7) and the blur radius.
    float blending = 0.0f;
    float dilution = 0.0f;
    float bleed = 0.0f;
    float strength = 1.0f;
    int blurRadius = 6;  // [0, 32]
    // §4, §14.2: a tip mask (LINEAR, CLAMP_TO_EDGE, turned and scaled with the ellipse) and a grain
    // mask repeated over the canvas, one texel per document pixel.
    std::shared_ptr<const core::MaskImage> tipMask;
    std::shared_ptr<const core::MaskImage> grainMask;
};

/// The §4.1 constants of a profile with an ink colour (linear premultiplied, alpha = colour alpha).
BrushSetup brushSetupFor(const core::BrushProfile& profile, const std::array<float, 4>& inkPma, bool erase);

/// What a frame composes: the tree and the committed tiles of its raster layers.
struct LayerStack {
    const core::LayerTree* tree = nullptr;
    std::function<TileRef(std::uint64_t layer, int tx, int ty)> tiles;
};

/// The move preview of one layer (§10.1): `base` stays, `floating` is composed over it shifted by
/// (dx, dy). The caller bumps `epoch` whenever the offset changes.
struct FloatingLayer {
    std::uint64_t layer = 0;
    std::map<std::pair<int, int>, TileRef> base;
    std::map<std::pair<int, int>, TileRef> floating;
    int dx = 0;
    int dy = 0;
    std::uint64_t epoch = 0;
};

/// One filter run (§9): read `source`, show and publish the result as `target` (the source itself, or a
/// new layer for "to a new layer"). Only `tiles` are computed; the selection set with setSelection()
/// weights the final mix.
struct FxRequest {
    core::FxSettings settings;
    std::uint64_t source = 0;
    std::uint64_t target = 0;
    bool alphaLock = false;
    std::vector<std::pair<int, int>> tiles;
};

/// Where the document lands in the window for one frame.
struct DocumentFrame {
    int clientWidth = 0;  // physical px
    int clientHeight = 0;
    int viewportX0 = 0;  // physical px, top-left origin; everything is clipped to this rectangle
    int viewportY0 = 0;
    int viewportX1 = 0;
    int viewportY1 = 0;
    double originX = 0.0;  // client px of document (0, 0)
    double originY = 0.0;
    double pixelsPerDocPx = 1.0;
    std::array<float, 3> paper{1.0f, 1.0f, 1.0f};  // linear
};

class TileRenderer {
public:
    /// Compiles every program; throws std::runtime_error when a stage fails.
    TileRenderer(const gl::Context& context, int documentWidth, int documentHeight);
    ~TileRenderer();
    TileRenderer(const TileRenderer&) = delete;
    TileRenderer& operator=(const TileRenderer&) = delete;

    [[nodiscard]] int documentWidth() const noexcept { return width_; }
    [[nodiscard]] int documentHeight() const noexcept { return height_; }
    /// New or opened document. Not during a stroke. Drops the composed tiles.
    void setDocumentSize(int width, int height);

    /// A version that is not on the GPU yet (opened from a file).
    static TileRef makeVersion(std::shared_ptr<const core::TileData> data);
    /// Drops the GPU copy of a version that is no longer displayed (history only).
    void evict(const TileVersion& version);

    // ── Stroke transaction ──
    /// `committed` looks up the committed tiles of setup.layer.
    void beginStroke(const BrushSetup& setup, TileLookup committed);
    void applyDabs(std::span<const core::Dab> dabs);  // final dabs, in stroke order
    void setTail(std::span<const core::Dab> dabs);    // replaces the provisional tail
    void replay(std::span<const core::Dab> dabs);     // the whole stroke again from the base
    [[nodiscard]] std::vector<TileChange> commitStroke();
    void cancelStroke();
    [[nodiscard]] bool strokeActive() const noexcept { return strokeActive_; }
    [[nodiscard]] std::size_t strokeTileCount() const noexcept { return tiles_.size(); }

    // ── Selection and move preview ──
    /// The selection strokes respect (§10.1); nullptr: none, writing is allowed everywhere.
    void setSelection(const core::SelectionMask* mask);
    /// A layer shown as a move preview, or nullptr. The object must outlive its use.
    void setFloating(const FloatingLayer* floating);

    // ── Filters (§9) ──
    /// Computes the preview of a filter on the GPU, replacing any previous one; it is shown in place of
    /// the target layer's tiles. `committed` looks up the source layer's tiles.
    void previewFx(const FxRequest& request, const TileLookup& committed);
    /// The preview as canonical tiles of the target (OK publishes them as one command); clears it.
    [[nodiscard]] std::vector<TileChange> commitFx(const TileLookup& targetCommitted);
    void cancelFx();
    [[nodiscard]] bool fxActive() const noexcept { return fx_.active; }

    // ── Presentation ──
    void drawDocument(const DocumentFrame& frame, const LayerStack& stack);
    /// The selection boundary as a 1-px dashed line, optionally shifted by whole document pixels.
    void drawSelection(const DocumentFrame& frame, const core::SelectionMask& mask, int dx = 0, int dy = 0);
    void drawBrushRing(const DocumentFrame& frame, double centerX, double centerY, double radiusPx);

    // ── Read-back (tests) ──
    /// The GPU copy of a version as RGBA floats, row 0 = top: GPU and RAM must agree.
    [[nodiscard]] std::vector<float> readResident(const TileVersion& version);
    /// One composed tile as RGBA floats, row 0 = top, without the cache.
    [[nodiscard]] std::vector<float> readComposite(const LayerStack& stack, int tx, int ty);
    /// Composed tiles kept for display.
    [[nodiscard]] std::size_t cachedTiles() const noexcept { return cache_.size(); }

private:
    struct StrokeTile {
        TileRef before;
        bool ready = false;    // final buffers allocated
        bool touched = false;  // received at least one final stamp
        GLuint weight = 0;     // max mode: R16F
        GLuint working = 0;    // build mode: RGBA16F
        bool tail = false;     // preview buffers valid
        GLuint previewWeight = 0;
        GLuint previewWorking = 0;
    };
    /// A tile image during composition; owned images go back to the pool when dropped.
    struct Image {
        GLuint texture = 0;
        bool owned = false;
    };
    struct CacheEntry {
        GLuint texture = 0;
        std::vector<std::uint64_t> signature;
        std::uint64_t lastUse = 0;
    };
    struct PixelRect {
        int x0, y0, x1, y1;  // document px, half-open
    };
    /// Work textures of one wet stamp, all the size of its region.
    struct WetTargets {
        GLuint in = 0, out = 0, aux = 0;  // RGBA16F
        GLuint mask = 0;                  // R16F, the §4.1 weight
        GLuint selection = 0;             // R8 of ones until selections exist
        int width = 0, height = 0;
    };
    struct WetState {
        bool started = false;
        core::V previous{};
        std::array<float, 4> reservoir{};  // H, linear PMA
    };

    void stampBatch(std::span<const core::Dab> dabs, bool preview);
    void wetStamp(const core::Dab& dab);
    void ensureWetTargets(int width, int height);
    /// R8 texture of a mask tile, uploaded on first use.
    GLuint maskTexture(const core::MaskRef& tile);
    /// What the stamp program reads as `selection` for tile (tx, ty).
    GLuint selectionFor(int tx, int ty);
    void assembleSelection(const PixelRect& region);
    Image floatingContent(const LayerStack& stack, int tx, int ty);
    GLuint fxTile(const FxRequest& request, const TileLookup& committed, int tx, int ty);
    void ensureFxTargets(int width, int height);
    void assembleFrom(const TileLookup& lookup, const PixelRect& region, GLuint target);
    void assembleRegion(const PixelRect& region, GLuint target);
    void blitRect(GLuint source, int sx, int sy, GLuint target, int dx, int dy, int width, int height) const;
    [[nodiscard]] bool wet() const noexcept { return setup_.stroke != StrokeKind::Dry; }
    void drawTile(const core::TileKey& key, StrokeTile& tile, std::span<const std::uint32_t> dabIndices,
                  std::span<const core::Dab> dabs, bool preview);
    void setDabUniforms(const core::Dab& dab) const;
    StrokeTile& prepareTile(const core::TileKey& key, bool preview);
    void releasePreview(StrokeTile& tile);
    void releaseTile(StrokeTile& tile);
    [[nodiscard]] PixelRect dabRect(const core::Dab& dab) const;
    /// Texture of a version on this renderer, uploading it from RAM when needed.
    GLuint resident(const TileVersion& version);
    /// Reads a stroke result back, canonicalises it and wraps it as a version owning `texture`.
    TileRef finishTile(const core::TileKey& key, GLuint texture, const TileRef& before);

    // Composition (§7).
    Image composeChildren(const LayerStack& stack, std::uint64_t parent, int tx, int ty);
    Image content(const LayerStack& stack, const core::LayerNode& node, int tx, int ty);
    Image rasterContent(const LayerStack& stack, std::uint64_t layer, int tx, int ty);
    Image blendInto(Image dst, Image src, float opacity, int mode);
    Image unitCoverage(const Image& base);
    Image clipToSilhouette(Image chain, const Image& base, float opacity);
    void drop(Image& image);
    /// The composed tile for display, from the cache when its signature still holds. The returned
    /// image is owned only when it could not be cached.
    Image displayTile(const LayerStack& stack, int tx, int ty);
    void signatureOf(const LayerStack& stack, int tx, int ty, std::vector<std::uint64_t>& out);
    void clearCache();

    void attach(GLuint texture) const;
    void runPass(GLuint target) const;  // full-tile pass of the current program into `target`
    void clearTexture(GLuint texture, float value) const;
    void copyTexture(GLuint source, GLuint target) const;
    void bindTexture(int unit, GLuint texture) const;

    const gl::Api& api_;
    std::shared_ptr<TexturePool> pool_;
    std::weak_ptr<const void> lifetime_;  // the context's; a renderer outliving it skips GL teardown
    int width_;
    int height_;

    std::unique_ptr<gl::Program> stamp_;
    std::unique_ptr<gl::Program> dabComposite_;
    std::unique_ptr<gl::Program> resolve_;
    std::unique_ptr<gl::Program> display_;
    std::unique_ptr<gl::Program> ring_;
    std::unique_ptr<gl::Program> composite_;
    std::unique_ptr<gl::Program> unitCoverage_;
    std::unique_ptr<gl::Program> clip_;
    std::unique_ptr<gl::Program> wetProgram_;
    std::unique_ptr<gl::Program> gaussian_;
    std::unique_ptr<gl::Program> reservoir_;
    std::unique_ptr<gl::Program> blurMix_;
    std::unique_ptr<gl::Program> lockApply_;
    std::unique_ptr<gl::Program> selectionOutline_;
    std::unique_ptr<gl::Program> toneCurve_;
    std::unique_ptr<gl::Program> unsharp_;
    std::unique_ptr<gl::Program> chromatic_;
    std::unique_ptr<gl::Program> animeLines_;
    std::unique_ptr<gl::Program> fxMix_;

    struct StampUniforms {
        GLint originPx, centerPx, radius, aspect, hardness, angle, inkWeight;
        GLint grainPx, grainAmount, periodPx, density, toneAngle, kind, seed;
    } stampU_{};
    struct DabCompositeUniforms {
        GLint inkPma, erase, alphaLock;
    } dabU_{};
    struct ResolveUniforms {
        GLint inkPma, erase, hasBase, alphaLock;
    } resolveU_{};
    struct DisplayUniforms {
        GLint rectNdc, uvRect, paper, opacity;
    } displayU_{};
    struct RingUniforms {
        GLint rectNdc, uvRect, center, radius;
    } ringU_{};
    struct CompositeUniforms {
        GLint opacity, mode;
    } compositeU_{};
    GLint clipOpacityU_ = -1;
    struct WetUniforms {
        GLint deltaPx, reservoir, freshInk, radius, blending, dilution, bleed, strength, watercolor, emitSource;
    } wetU_{};
    struct GaussianUniforms {
        GLint direction, radiusPx;
    } gaussianU_{};
    struct ReservoirUniforms {
        GLint centerPx, halfRadius;
    } reservoirU_{};
    GLint blurStrengthU_ = -1;
    struct FxUniforms {
        GLint toneDiscrete, unsharpGain, unsharpThreshold, chromaticShift;
        GLint animeBlack, animeWhite, animeMid, animeEdge, mixAmount, mixToNew;
    } fxU_{};
    struct OutlineUniforms {
        GLint rectNdc, uvRect, tileOrigin, scale, screenHeight, has;
    } outlineU_{};

    GLuint targetFbo_ = 0;  // re-attached per tile
    GLuint tempFbo_ = 0;    // permanently attached to tempWeight_
    GLuint readFbo_ = 0;    // blit and read-back source
    GLuint tempWeight_ = 0;
    GLuint zeroTile_ = 0;   // 128² RGBA16F of zeros: "nothing below"
    GLuint selection_ = 0;  // 128² R8 of ones: "no selection" (§4.1 neutral value 1)
    GLuint pattern_ = 0;    // 1×1 R8 of one, REPEAT/LINEAR (§4.1 when no user mask)
    GLuint tipMask_ = 0;    // 1×1 R8 of one
    GLuint nearestSampler_ = 0;
    GLuint linearSampler_ = 0;

    bool strokeActive_ = false;
    BrushSetup setup_{};
    TileLookup committed_;
    std::uint64_t strokeEpoch_ = 0;  // changes whenever the stroke's pixels may have changed
    WetState wet_;
    WetTargets wetTex_;
    GLuint reservoirTex_ = 0;  // 1 × 1 RGBA32F

    std::optional<core::SelectionMask> strokeMask_;  // the selection strokes respect
    GLuint zeroMask_ = 0;                            // 128² R8 of zeros: "outside the selection"
    std::map<const core::MaskTile*, std::pair<core::MaskRef, GLuint>> maskTextures_;
    const FloatingLayer* floating_ = nullptr;

    struct FxState {
        bool active = false;
        std::uint64_t target = 0;
        bool replaces = true;  // target == source: tiles without a preview show the committed ones
        std::map<std::pair<int, int>, GLuint> tiles;
        std::uint64_t epoch = 0;
    } fx_;
    struct FxTargets {
        GLuint in = 0, a = 0, b = 0;  // RGBA16F, the region with its halo
        int width = 0, height = 0;
    } fxTex_;
    GLuint curveTex_ = 0;  // 256 × 1 RGBA32F
    GLuint userTip_ = 0;      // the stroke's tip mask, R8
    GLuint userPattern_ = 0;  // the stroke's grain mask, R8, REPEAT
    const core::MaskImage* tipUploaded_ = nullptr;
    const core::MaskImage* patternUploaded_ = nullptr;
    GLuint activeTip_ = 0;
    GLuint activePattern_ = 0;
    std::unordered_map<core::TileKey, StrokeTile, core::TileKeyHash> tiles_;
    std::unordered_map<core::TileKey, std::vector<std::uint32_t>, core::TileKeyHash> batch_;
    std::vector<core::TileKey> batchOrder_;

    // Composed tiles for display, keyed by (tx, ty); at most kCacheTiles (128 MiB of RGBA16F).
    static constexpr std::size_t kCacheTiles = 1024;
    std::unordered_map<std::uint64_t, CacheEntry> cache_;
    std::uint64_t frame_ = 0;
    std::uint64_t treeHash_ = 0;
    std::vector<std::uint64_t> rasters_;  // raster layers of this frame's tree, in preorder
    std::vector<std::uint64_t> signature_;
};

}  // namespace chroma::render

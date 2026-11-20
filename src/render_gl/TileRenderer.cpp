#include "render_gl/TileRenderer.hpp"

#include "core/BrushProfile.hpp"
#include "core/Log.hpp"
#include "render_gl/Shaders.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

namespace chroma::render {
namespace {

// Texture units. Each program reads fixed units, so bindings never alias the attached target.
// Unit 7 is only for creating and uploading textures, never sampled.
constexpr int kUnitSelection = 0;
constexpr int kUnitPattern = 1;
constexpr int kUnitTipMask = 2;
constexpr int kUnitWeight = 3;
constexpr int kUnitBase = 4;
constexpr int kUnitLayer = 5;
constexpr int kUnitLock = 6;
constexpr int kUnitUpload = 7;

constexpr std::uint64_t kStrokeToken = std::uint64_t{1} << 63;  // signature token of a live stroke tile
constexpr std::uint64_t kFloatToken = std::uint64_t{1} << 62;   // ... of a layer under a move preview
constexpr std::uint64_t kFxToken = std::uint64_t{1} << 61;      // ... of a layer under a filter preview

std::atomic<std::uint64_t> g_versionSerial{0};

// FNV-1a over 64-bit words.
void hashInto(std::uint64_t& h, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        h ^= (value >> (8 * i)) & 0xFF;
        h *= 0x100000001B3ull;
    }
}

void setNearestClamp() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl::kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl::kClampToEdge);
}

GLuint makeR8(int size, GLint wrap, GLint filter) {
    const std::vector<unsigned char> ones(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 255);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kR8), size, size, 0, GL_RED, GL_UNSIGNED_BYTE, ones.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return texture;
}

}  // namespace

BrushSetup brushSetupFor(const core::BrushProfile& profile, const std::array<float, 4>& inkPma, bool erase) {
    BrushSetup setup;
    setup.erase = erase;
    setup.maxMode = profile.ink.accumulate == core::Accumulate::Max;
    setup.kind = core::tipKind(profile);
    setup.hardness = static_cast<float>(profile.tip.hardness);
    setup.inkPma = inkPma;
    if (profile.grain) {
        setup.grainPx = static_cast<float>(profile.grain->grainPx);
        setup.grainAmount = static_cast<float>(profile.grain->grainAmount);
        setup.seed = profile.grain->seed;
    }
    if (profile.tone) {
        setup.periodPx = static_cast<float>(profile.tone->periodPx);
        setup.density = static_cast<float>(profile.tone->density);
        setup.toneAngle = static_cast<float>(profile.tone->toneAngleDeg * std::numbers::pi / 180.0);
    }
    if (profile.wet && !erase) {
        setup.stroke = StrokeKind::Watercolor;
        setup.maxMode = false;  // §6.1: watercolor accumulates as build
        setup.blending = static_cast<float>(profile.wet->blending);
        setup.dilution = static_cast<float>(profile.wet->dilution);
        setup.bleed = static_cast<float>(profile.wet->bleed);
        setup.strength = static_cast<float>(profile.wet->strength);
    }
    return setup;
}

// ── Texture pool ─────────────────────────────────────────────────────────────
// 128² textures are recycled: a stroke allocates and frees many per frame. Versions hold only a weak
// reference: when the renderer goes (device loss), its textures go with the context and versions
// upload again into the next renderer.

class TexturePool {
public:
    TexturePool(const gl::Api& api, std::weak_ptr<const void> lifetime) : api_(api), lifetime_(std::move(lifetime)) {}

    ~TexturePool() {
        if (lifetime_.expired()) return;  // the context took its objects with it
        for (std::vector<GLuint>* list : {&rgba_, &red_}) {
            if (!list->empty()) glDeleteTextures(static_cast<GLsizei>(list->size()), list->data());
        }
    }

    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    /// 128 × 128, NEAREST, CLAMP_TO_EDGE; contents undefined. Leaves the texture bound on unit 7.
    GLuint acquire(GLenum internalFormat) {
        std::vector<GLuint>& list = internalFormat == gl::kRgba16f ? rgba_ : red_;
        if (!list.empty()) {
            const GLuint texture = list.back();
            list.pop_back();
            return texture;
        }
        GLuint texture = 0;
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        setNearestClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), kTileSize, kTileSize, 0,
                     internalFormat == gl::kRgba16f ? GL_RGBA : GL_RED, GL_FLOAT, nullptr);
        return texture;
    }

    void release(GLenum internalFormat, GLuint texture) {
        if (texture == 0) return;
        (internalFormat == gl::kRgba16f ? rgba_ : red_).push_back(texture);
    }

private:
    const gl::Api& api_;
    std::weak_ptr<const void> lifetime_;
    std::vector<GLuint> rgba_;
    std::vector<GLuint> red_;
};

TileVersion::TileVersion(std::shared_ptr<const core::TileData> data)
    : data_(std::move(data)), serial_(++g_versionSerial) {}

TileVersion::~TileVersion() {
    if (texture_ == 0) return;
    if (const std::shared_ptr<TexturePool> pool = pool_.lock()) pool->release(gl::kRgba16f, texture_);
}

// ── Construction ─────────────────────────────────────────────────────────────

TileRenderer::TileRenderer(const gl::Context& context, int documentWidth, int documentHeight)
    : api_(context.api()),
      pool_(std::make_shared<TexturePool>(context.api(), context.lifetime())),
      lifetime_(context.lifetime()),
      width_(documentWidth),
      height_(documentHeight) {
    setDocumentSize(documentWidth, documentHeight);
    using namespace gl::shaders;
    stamp_ = std::make_unique<gl::Program>(context, "stamp", kFullscreenVertex, kStampFragment);
    dabComposite_ = std::make_unique<gl::Program>(context, "dab composite", kFullscreenVertex, kDabCompositeFragment);
    resolve_ = std::make_unique<gl::Program>(context, "resolve", kFullscreenVertex, kResolveFragment);
    display_ = std::make_unique<gl::Program>(context, "display", kQuadVertex, kDisplayFragment);
    boxDisplay_ = std::make_unique<gl::Program>(context, "box display", kQuadVertex, kBoxDisplayFragment);
    boxResolve_ = std::make_unique<gl::Program>(context, "box resolve", kQuadVertex, kBoxResolveFragment);
    ring_ = std::make_unique<gl::Program>(context, "ring", kQuadVertex, kRingFragment);
    composite_ = std::make_unique<gl::Program>(context, "composite", kFullscreenVertex, kCompositeFragment);
    unitCoverage_ = std::make_unique<gl::Program>(context, "unit coverage", kFullscreenVertex, kUnitCoverageFragment);
    clip_ = std::make_unique<gl::Program>(context, "clip", kFullscreenVertex, kClipFragment);
    wetProgram_ = std::make_unique<gl::Program>(context, "wet stamp", kFullscreenVertex, kWetFragment);
    gaussian_ = std::make_unique<gl::Program>(context, "gaussian", kFullscreenVertex, kGaussianFragment);
    reservoir_ = std::make_unique<gl::Program>(context, "reservoir", kFullscreenVertex, kReservoirFragment);
    blurMix_ = std::make_unique<gl::Program>(context, "blur mix", kFullscreenVertex, kBlurMixFragment);
    lockApply_ = std::make_unique<gl::Program>(context, "lock apply", kFullscreenVertex, kLockApplyFragment);
    selectionOutline_ = std::make_unique<gl::Program>(context, "selection outline", kQuadVertex, kSelectionFragment);
    toneCurve_ = std::make_unique<gl::Program>(context, "tone curve", kFullscreenVertex, kToneCurveFragment);
    unsharp_ = std::make_unique<gl::Program>(context, "unsharp mask", kFullscreenVertex, kUnsharpFragment);
    chromatic_ = std::make_unique<gl::Program>(context, "chromatic aberration", kFullscreenVertex, kChromaticFragment);
    animeLines_ = std::make_unique<gl::Program>(context, "anime lines", kFullscreenVertex, kAnimeLinesFragment);
    fxMix_ = std::make_unique<gl::Program>(context, "fx mix", kFullscreenVertex, kFxMixFragment);

    stamp_->use();
    api_.uniform1i(stamp_->uniform("selection"), kUnitSelection);
    api_.uniform1i(stamp_->uniform("pattern"), kUnitPattern);
    api_.uniform1i(stamp_->uniform("tipMask"), kUnitTipMask);
    api_.uniform1i(stamp_->uniform("useTipMask"), 0);
    stampU_ = {stamp_->uniform("originPx"), stamp_->uniform("centerPx"),    stamp_->uniform("radius"),
               stamp_->uniform("aspect"),   stamp_->uniform("hardness"),    stamp_->uniform("angle"),
               stamp_->uniform("inkWeight"), stamp_->uniform("grainPx"),    stamp_->uniform("grainAmount"),
               stamp_->uniform("periodPx"), stamp_->uniform("density"),     stamp_->uniform("toneAngle"),
               stamp_->uniform("kind"),     stamp_->uniform("seed")};

    dabComposite_->use();
    api_.uniform1i(dabComposite_->uniform("weightTex"), kUnitWeight);
    api_.uniform1i(dabComposite_->uniform("lockTex"), kUnitLock);
    dabU_ = {dabComposite_->uniform("inkPma"), dabComposite_->uniform("erase"), dabComposite_->uniform("alphaLock")};

    resolve_->use();
    api_.uniform1i(resolve_->uniform("baseTex"), kUnitBase);
    api_.uniform1i(resolve_->uniform("weightTex"), kUnitWeight);
    resolveU_ = {resolve_->uniform("inkPma"), resolve_->uniform("erase"), resolve_->uniform("hasBase"),
                 resolve_->uniform("alphaLock")};

    display_->use();
    api_.uniform1i(display_->uniform("layerTex"), kUnitLayer);
    displayU_ = {display_->uniform("rectNdc"), display_->uniform("uvRect"), display_->uniform("paper"),
                 display_->uniform("opacity")};
    boxDisplay_->use();
    api_.uniform1i(boxDisplay_->uniform("layerTex"), kUnitLayer);
    boxU_ = {boxDisplay_->uniform("rectNdc"),      boxDisplay_->uniform("uvRect"),   boxDisplay_->uniform("originPx"),
             boxDisplay_->uniform("clientHeight"), boxDisplay_->uniform("docPerPx"), boxDisplay_->uniform("tileOrigin")};
    boxResolve_->use();
    api_.uniform1i(boxResolve_->uniform("accum"), kUnitLayer);
    boxResolveU_ = {boxResolve_->uniform("rectNdc"), boxResolve_->uniform("uvRect"), boxResolve_->uniform("paper")};

    composite_->use();
    api_.uniform1i(composite_->uniform("bottomImage"), kUnitBase);
    api_.uniform1i(composite_->uniform("topImage"), kUnitLayer);
    compositeU_ = {composite_->uniform("opacity"), composite_->uniform("mode")};
    unitCoverage_->use();
    api_.uniform1i(unitCoverage_->uniform("image"), kUnitLayer);
    clip_->use();
    api_.uniform1i(clip_->uniform("chainImage"), kUnitBase);
    api_.uniform1i(clip_->uniform("baseImage"), kUnitLayer);
    clipOpacityU_ = clip_->uniform("opacity");

    wetProgram_->use();
    api_.uniform1i(wetProgram_->uniform("imageIn"), kUnitBase);
    api_.uniform1i(wetProgram_->uniform("brushMask"), kUnitWeight);
    wetU_ = {wetProgram_->uniform("deltaPx"),  wetProgram_->uniform("reservoir"), wetProgram_->uniform("freshInk"),
             wetProgram_->uniform("radius"),   wetProgram_->uniform("blending"),  wetProgram_->uniform("dilution"),
             wetProgram_->uniform("bleed"),    wetProgram_->uniform("strength"),  wetProgram_->uniform("watercolor"),
             wetProgram_->uniform("emitSource")};
    gaussian_->use();
    api_.uniform1i(gaussian_->uniform("imageIn"), kUnitBase);
    gaussianU_ = {gaussian_->uniform("direction"), gaussian_->uniform("radiusPx")};
    reservoir_->use();
    api_.uniform1i(reservoir_->uniform("imageIn"), kUnitBase);
    reservoirU_ = {reservoir_->uniform("centerPx"), reservoir_->uniform("halfRadius")};
    blurMix_->use();
    api_.uniform1i(blurMix_->uniform("imageIn"), kUnitBase);
    api_.uniform1i(blurMix_->uniform("blurred"), kUnitLayer);
    api_.uniform1i(blurMix_->uniform("brushMask"), kUnitWeight);
    blurStrengthU_ = blurMix_->uniform("strength");
    lockApply_->use();
    api_.uniform1i(lockApply_->uniform("original"), kUnitBase);
    api_.uniform1i(lockApply_->uniform("candidate"), kUnitLayer);
    selectionOutline_->use();
    api_.uniform1i(selectionOutline_->uniform("maskC"), kUnitSelection);
    api_.uniform1i(selectionOutline_->uniform("maskL"), kUnitPattern);
    api_.uniform1i(selectionOutline_->uniform("maskR"), kUnitTipMask);
    api_.uniform1i(selectionOutline_->uniform("maskU"), kUnitWeight);
    api_.uniform1i(selectionOutline_->uniform("maskD"), kUnitBase);
    toneCurve_->use();
    api_.uniform1i(toneCurve_->uniform("imageIn"), kUnitBase);
    api_.uniform1i(toneCurve_->uniform("curveTable"), kUnitLayer);
    unsharp_->use();
    api_.uniform1i(unsharp_->uniform("imageIn"), kUnitBase);
    api_.uniform1i(unsharp_->uniform("blurred"), kUnitLayer);
    chromatic_->use();
    api_.uniform1i(chromatic_->uniform("imageIn"), kUnitBase);
    animeLines_->use();
    api_.uniform1i(animeLines_->uniform("imageIn"), kUnitBase);
    fxMix_->use();
    api_.uniform1i(fxMix_->uniform("original"), kUnitBase);
    api_.uniform1i(fxMix_->uniform("filtered"), kUnitLayer);
    api_.uniform1i(fxMix_->uniform("selection"), kUnitSelection);
    fxU_ = {toneCurve_->uniform("discrete"),   unsharp_->uniform("gain"),        unsharp_->uniform("threshold"),
            chromatic_->uniform("shiftPx"),    animeLines_->uniform("blackPoint"), animeLines_->uniform("whitePoint"),
            animeLines_->uniform("mid"),       animeLines_->uniform("edgeGain"), fxMix_->uniform("amount"),
            fxMix_->uniform("toNewLayer")};
    selectionOutline_->use();
    outlineU_ = {selectionOutline_->uniform("rectNdc"), selectionOutline_->uniform("uvRect"),
                 selectionOutline_->uniform("tileOrigin"), selectionOutline_->uniform("scale"),
                 selectionOutline_->uniform("screenHeight"), -1};

    ringU_ = {ring_->uniform("rectNdc"), ring_->uniform("uvRect"), ring_->uniform("center"), ring_->uniform("radius")};
    api_.useProgram(0);

    api_.genFramebuffers(1, &targetFbo_);
    api_.genFramebuffers(1, &tempFbo_);
    api_.genFramebuffers(1, &readFbo_);
    tempWeight_ = pool_->acquire(gl::kR16f);
    api_.bindFramebuffer(gl::kFramebuffer, tempFbo_);
    api_.framebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, tempWeight_, 0);
    const GLenum status = api_.checkFramebufferStatus(gl::kFramebuffer);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    if (status != gl::kFramebufferComplete) throw std::runtime_error("R16F stamp target incomplete");
    zeroTile_ = pool_->acquire(gl::kRgba16f);
    clearTexture(zeroTile_, 0.0f);
    api_.activeTexture(gl::kTexture0 + kUnitUpload);
    glGenTextures(1, &reservoirTex_);
    glBindTexture(GL_TEXTURE_2D, reservoirTex_);
    setNearestClamp();
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kRgba32f), 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
    api_.bindFramebuffer(gl::kFramebuffer, 0);

    api_.activeTexture(gl::kTexture0 + kUnitUpload);
    selection_ = makeR8(kTileSize, gl::kClampToEdge, GL_NEAREST);
    {
        const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(kTileSize) * kTileSize, 0);
        glGenTextures(1, &zeroMask_);
        glBindTexture(GL_TEXTURE_2D, zeroMask_);
        setNearestClamp();
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kR8), kTileSize, kTileSize, 0, GL_RED, GL_UNSIGNED_BYTE, zeros.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    pattern_ = makeR8(1, GL_REPEAT, GL_LINEAR);
    tipMask_ = makeR8(1, gl::kClampToEdge, GL_LINEAR);

    api_.genSamplers(1, &nearestSampler_);
    api_.genSamplers(1, &linearSampler_);
    for (const auto& [sampler, filter] : {std::pair{nearestSampler_, GL_NEAREST}, std::pair{linearSampler_, GL_LINEAR}}) {
        api_.samplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
        api_.samplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
        api_.samplerParameteri(sampler, GL_TEXTURE_WRAP_S, static_cast<GLint>(gl::kClampToEdge));
        api_.samplerParameteri(sampler, GL_TEXTURE_WRAP_T, static_cast<GLint>(gl::kClampToEdge));
    }
}

TileRenderer::~TileRenderer() {
    if (lifetime_.expired()) return;  // the context went first: its objects went with it
    cancelStroke();
    cancelFx();
    clearCache();
    const GLuint fxTextures[] = {fxTex_.in, fxTex_.a, fxTex_.b, curveTex_, userTip_, userPattern_};
    glDeleteTextures(6, fxTextures);
    pool_->release(gl::kRgba16f, zeroTile_);
    const GLuint wetTextures[] = {wetTex_.in, wetTex_.out, wetTex_.aux, wetTex_.mask, wetTex_.selection, reservoirTex_, zeroMask_};
    glDeleteTextures(7, wetTextures);
    for (auto& [key, entry] : maskTextures_) glDeleteTextures(1, &entry.second);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    const GLuint framebuffers[] = {targetFbo_, tempFbo_, readFbo_};
    api_.deleteFramebuffers(3, framebuffers);
    const GLuint textures[] = {selection_, pattern_, tipMask_};
    glDeleteTextures(3, textures);
    if (accumFbo_) api_.deleteFramebuffers(1, &accumFbo_);
    if (accumTex_) glDeleteTextures(1, &accumTex_);
    const GLuint samplers[] = {nearestSampler_, linearSampler_};
    api_.deleteSamplers(2, samplers);
    pool_->release(gl::kR16f, tempWeight_);
}

void TileRenderer::setDocumentSize(int width, int height) {
    if (width < 1 || height < 1 || width > 32768 || height > 32768) {
        throw std::invalid_argument("document size must be 1..32768 px per axis");  // spec §2
    }
    if (strokeActive_) cancelStroke();
    if (width != width_ || height != height_) clearCache();
    width_ = width;
    height_ = height;
}

// ── Versions and residency ───────────────────────────────────────────────────

TileRef TileRenderer::makeVersion(std::shared_ptr<const core::TileData> data) {
    return std::make_shared<const TileVersion>(std::move(data));
}

GLuint TileRenderer::resident(const TileVersion& version) {
    if (version.texture_ != 0 && version.pool_.lock() == pool_) return version.texture_;
    if (version.texture_ != 0) {
        // A copy on another live renderer goes back to it; a copy on a lost context is just forgotten.
        if (const std::shared_ptr<TexturePool> other = version.pool_.lock()) other->release(gl::kRgba16f, version.texture_);
    }
    const GLuint texture = pool_->acquire(gl::kRgba16f);
    api_.activeTexture(gl::kTexture0 + kUnitUpload);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTileSize, kTileSize, GL_RGBA, gl::kHalfFloat, version.data().halfs.data());
    version.texture_ = texture;
    version.pool_ = pool_;
    return texture;
}

void TileRenderer::evict(const TileVersion& version) {
    if (version.texture_ == 0 || version.pool_.lock() != pool_) return;
    pool_->release(gl::kRgba16f, version.texture_);
    version.texture_ = 0;
    version.pool_.reset();
}

TileRef TileRenderer::finishTile(const core::TileKey& key, GLuint texture, const TileRef& before) {
    auto data = std::make_shared<core::TileData>();
    api_.bindFramebuffer(gl::kReadFramebuffer, readFbo_);
    api_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, texture, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, kTileSize, kTileSize, GL_RGBA, gl::kHalfFloat, data->halfs.data());
    api_.bindFramebuffer(gl::kReadFramebuffer, 0);

    // GPU arithmetic can leave rgb a hair above a, or −0: store the canonical form, and keep the GPU
    // copy identical to it.
    if (core::canonicalize(*data, core::tileExtent(width_, height_, key.tx, key.ty))) {
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTileSize, kTileSize, GL_RGBA, gl::kHalfFloat, data->halfs.data());
    }
    if (core::isTransparent(*data)) {  // spec §10.2: a transparent tile is the absence of an entry
        pool_->release(gl::kRgba16f, texture);
        return nullptr;
    }
    if (before && before->data() == *data) {  // equal canonical bytes collapse
        pool_->release(gl::kRgba16f, texture);
        return before;
    }
    auto version = std::make_shared<TileVersion>(std::move(data));
    version->texture_ = texture;
    version->pool_ = pool_;
    return version;
}

// ── GL helpers ───────────────────────────────────────────────────────────────

void TileRenderer::attach(GLuint texture) const {
    api_.bindFramebuffer(gl::kFramebuffer, targetFbo_);
    api_.framebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, texture, 0);
}

void TileRenderer::clearTexture(GLuint texture, float value) const {
    attach(texture);
    glViewport(0, 0, kTileSize, kTileSize);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(value, value, value, value);
    glClear(GL_COLOR_BUFFER_BIT);
}

void TileRenderer::copyTexture(GLuint source, GLuint target) const {
    api_.bindFramebuffer(gl::kReadFramebuffer, readFbo_);
    api_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, source, 0);
    api_.bindFramebuffer(gl::kDrawFramebuffer, targetFbo_);
    api_.framebufferTexture2D(gl::kDrawFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, target, 0);
    glDisable(GL_SCISSOR_TEST);  // glBlitFramebuffer honours the scissor test
    api_.blitFramebuffer(0, 0, kTileSize, kTileSize, 0, 0, kTileSize, kTileSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    api_.bindFramebuffer(gl::kReadFramebuffer, 0);
}

void TileRenderer::bindTexture(int unit, GLuint texture) const {
    api_.activeTexture(gl::kTexture0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, texture);
}

// ── Stroke transaction ───────────────────────────────────────────────────────

void TileRenderer::beginStroke(const BrushSetup& setup, TileLookup committed) {
    if (strokeActive_) cancelStroke();
    setup_ = setup;
    if (wet()) setup_.maxMode = false;  // wet strokes work on RGBA copies of the layer
    // Masks of this stroke: uploaded once per image, otherwise the neutral 1 × 1 textures.
    const auto upload = [&](GLuint& texture, const core::MaskImage& mask, GLenum wrap) {
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        if (texture == 0) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kR8), mask.width, mask.height, 0, GL_RED, GL_UNSIGNED_BYTE,
                     mask.r8.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    };
    if (setup_.tipMask && setup_.tipMask->width > 0) {
        if (tipUploaded_ != setup_.tipMask.get()) upload(userTip_, *setup_.tipMask, gl::kClampToEdge);
        tipUploaded_ = setup_.tipMask.get();
        activeTip_ = userTip_;
    } else {
        activeTip_ = tipMask_;
    }
    if (setup_.grainMask && setup_.grainMask->width > 0) {
        if (patternUploaded_ != setup_.grainMask.get()) upload(userPattern_, *setup_.grainMask, GL_REPEAT);
        patternUploaded_ = setup_.grainMask.get();
        activePattern_ = userPattern_;
    } else {
        activePattern_ = pattern_;
    }
    committed_ = std::move(committed);
    strokeActive_ = true;
    wet_ = {};
    ++strokeEpoch_;

    stamp_->use();
    api_.uniform1i(stamp_->uniform("useTipMask"), activeTip_ == userTip_ && userTip_ != 0 ? 1 : 0);
    api_.uniform1f(stampU_.hardness, setup_.hardness);
    api_.uniform1i(stampU_.kind, setup_.kind);
    api_.uniform1f(stampU_.grainPx, setup_.grainPx);
    api_.uniform1f(stampU_.grainAmount, setup_.grainAmount);
    api_.uniform1f(stampU_.periodPx, setup_.periodPx);
    api_.uniform1f(stampU_.density, setup_.density);
    api_.uniform1f(stampU_.toneAngle, setup_.toneAngle);
    api_.uniform1ui(stampU_.seed, setup_.seed);

    const auto& ink = setup_.inkPma;
    dabComposite_->use();
    api_.uniform4f(dabU_.inkPma, ink[0], ink[1], ink[2], ink[3]);
    api_.uniform1i(dabU_.erase, setup_.erase ? 1 : 0);
    api_.uniform1i(dabU_.alphaLock, setup_.alphaLock ? 1 : 0);
    resolve_->use();
    api_.uniform4f(resolveU_.inkPma, ink[0], ink[1], ink[2], ink[3]);
    api_.uniform1i(resolveU_.erase, setup_.erase ? 1 : 0);
    api_.uniform1i(resolveU_.alphaLock, setup_.alphaLock ? 1 : 0);
    api_.useProgram(0);
}

TileRenderer::PixelRect TileRenderer::dabRect(const core::Dab& dab) const {
    // Support of the §4.1 cover: ρ ≤ 1 + w along the major axis, plus one pixel for rounding.
    const double minor = dab.radius * std::clamp(dab.aspect, 0.05, 1.0);
    const double w = std::min(1.0, 0.5 / std::max(0.125, minor));
    const double extent = dab.radius * (1.0 + w) + 1.0;
    const auto clampAxis = [](double v, int limit) {
        return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(limit)));
    };
    return {clampAxis(std::floor(dab.center.x - extent), width_), clampAxis(std::floor(dab.center.y - extent), height_),
            clampAxis(std::ceil(dab.center.x + extent), width_), clampAxis(std::ceil(dab.center.y + extent), height_)};
}

TileRenderer::StrokeTile& TileRenderer::prepareTile(const core::TileKey& key, bool preview) {
    auto [it, inserted] = tiles_.try_emplace(key);
    StrokeTile& tile = it->second;
    if (inserted && committed_) tile.before = committed_(key.tx, key.ty);
    if (!tile.ready) {
        if (setup_.maxMode) {
            tile.weight = pool_->acquire(gl::kR16f);
            clearTexture(tile.weight, 0.0f);
        } else {
            tile.working = pool_->acquire(gl::kRgba16f);
            if (tile.before) {
                copyTexture(resident(*tile.before), tile.working);
            } else {
                clearTexture(tile.working, 0.0f);
            }
        }
        tile.ready = true;
    }
    if (preview && !tile.tail) {
        if (setup_.maxMode) {
            tile.previewWeight = pool_->acquire(gl::kR16f);
            copyTexture(tile.weight, tile.previewWeight);
        } else {
            tile.previewWorking = pool_->acquire(gl::kRgba16f);
            copyTexture(tile.working, tile.previewWorking);
        }
        tile.tail = true;
    }
    return tile;
}

void TileRenderer::releasePreview(StrokeTile& tile) {
    pool_->release(gl::kR16f, tile.previewWeight);
    pool_->release(gl::kRgba16f, tile.previewWorking);
    tile.previewWeight = 0;
    tile.previewWorking = 0;
    tile.tail = false;
}

void TileRenderer::releaseTile(StrokeTile& tile) {
    releasePreview(tile);
    pool_->release(gl::kR16f, tile.weight);
    pool_->release(gl::kRgba16f, tile.working);
    tile.weight = 0;
    tile.working = 0;
    tile.ready = false;
}

void TileRenderer::setDabUniforms(const core::Dab& dab) const {
    api_.uniform2f(stampU_.centerPx, static_cast<float>(dab.center.x), static_cast<float>(dab.center.y));
    api_.uniform1f(stampU_.radius, static_cast<float>(dab.radius));
    api_.uniform1f(stampU_.aspect, static_cast<float>(dab.aspect));
    api_.uniform1f(stampU_.angle, static_cast<float>(dab.angle));
    api_.uniform1f(stampU_.inkWeight, static_cast<float>(dab.inkWeight));
}

void TileRenderer::stampBatch(std::span<const core::Dab> dabs, bool preview) {
    if (!strokeActive_ || dabs.empty()) return;
    ++strokeEpoch_;
    if (wet()) {
        // §5: strictly stamp by stamp; each one reads what the previous one published.
        for (const core::Dab& dab : dabs) wetStamp(dab);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        api_.bindSampler(kUnitBase, 0);
        api_.useProgram(0);
        api_.bindFramebuffer(gl::kFramebuffer, 0);
        return;
    }

    // Group stamps by tile; each tile keeps the stroke order (build mode is order dependent).
    for (auto& entry : batch_) entry.second.clear();
    batchOrder_.clear();
    for (std::uint32_t i = 0; i < dabs.size(); ++i) {
        const core::Dab& dab = dabs[i];
        if (!(dab.radius > 0.0) || !(dab.inkWeight > 0.0)) continue;  // r = 0 is an empty stamp (§4)
        const PixelRect r = dabRect(dab);
        if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;  // entirely outside the document
        for (int ty = r.y0 / kTileSize; ty <= (r.y1 - 1) / kTileSize; ++ty) {
            for (int tx = r.x0 / kTileSize; tx <= (r.x1 - 1) / kTileSize; ++tx) {
                const core::TileKey key{setup_.layer, tx, ty};
                std::vector<std::uint32_t>& list = batch_[key];
                if (list.empty()) batchOrder_.push_back(key);
                list.push_back(i);
            }
        }
    }

    api_.useProgram(0);
    bindTexture(kUnitSelection, selection_);
    bindTexture(kUnitPattern, activePattern_ ? activePattern_ : pattern_);
    bindTexture(kUnitTipMask, activeTip_ ? activeTip_ : tipMask_);
    for (const core::TileKey& key : batchOrder_) {
        StrokeTile& tile = prepareTile(key, preview);
        if (!preview) tile.touched = true;
        drawTile(key, tile, batch_[key], dabs, preview);
    }
    glDisable(GL_BLEND);
    api_.blendEquation(gl::kFuncAdd);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
}

void TileRenderer::drawTile(const core::TileKey& key, StrokeTile& tile, std::span<const std::uint32_t> dabIndices,
                            std::span<const core::Dab> dabs, bool preview) {
    const int originX = key.tx * kTileSize;
    const int originY = key.ty * kTileSize;
    // Only the document part of an edge tile is ever written.
    const int limitX = std::min(kTileSize, width_ - originX);
    const int limitY = std::min(kTileSize, height_ - originY);
    const auto scissorFor = [&](const core::Dab& dab) {
        const PixelRect r = dabRect(dab);
        const int x0 = std::max(r.x0 - originX, 0);
        const int y0 = std::max(r.y0 - originY, 0);
        const int x1 = std::min(r.x1 - originX, limitX);
        const int y1 = std::min(r.y1 - originY, limitY);
        if (x1 <= x0 || y1 <= y0) return false;
        glScissor(x0, y0, x1 - x0, y1 - y0);  // tile rows grow with GL y: row 0 is the top
        return true;
    };

    GLuint target = 0;
    if (setup_.maxMode) {
        target = preview ? tile.previewWeight : tile.weight;
    } else {
        target = preview ? tile.previewWorking : tile.working;
    }
    bindTexture(kUnitSelection, selectionFor(key.tx, key.ty));  // §10.1: the selection limits writing
    attach(target);
    glViewport(0, 0, kTileSize, kTileSize);
    glEnable(GL_SCISSOR_TEST);

    if (setup_.maxMode) {
        stamp_->use();
        api_.uniform2i(stampU_.originPx, originX, originY);
        glEnable(GL_BLEND);
        api_.blendEquation(gl::kMax);  // per-pixel maximum weight across the gesture
        for (const std::uint32_t index : dabIndices) {
            const core::Dab& dab = dabs[index];
            if (!scissorFor(dab)) continue;
            setDabUniforms(dab);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        return;
    }

    // Build mode: weight into the temporary R16F, then composite into the working tile, one stamp
    // at a time. The target stays attached to targetFbo_; only the bound framebuffer switches.
    // Under Alpha Lock the stroke's base supplies a0 and the alpha channel is left alone (§7).
    bindTexture(kUnitLock, setup_.alphaLock && tile.before ? resident(*tile.before) : zeroTile_);
    bindTexture(kUnitWeight, tempWeight_);
    for (const std::uint32_t index : dabIndices) {
        const core::Dab& dab = dabs[index];
        api_.bindFramebuffer(gl::kFramebuffer, tempFbo_);
        if (!scissorFor(dab)) continue;
        glDisable(GL_BLEND);
        stamp_->use();
        api_.uniform2i(stampU_.originPx, originX, originY);
        setDabUniforms(dab);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        api_.bindFramebuffer(gl::kFramebuffer, targetFbo_);
        dabComposite_->use();
        glEnable(GL_BLEND);
        api_.blendEquation(gl::kFuncAdd);
        if (setup_.alphaLock) {
            api_.blendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        } else {
            glBlendFunc(setup_.erase ? GL_ZERO : GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}

void TileRenderer::applyDabs(std::span<const core::Dab> dabs) { stampBatch(dabs, false); }

// ── Wet stamps (§5) ──────────────────────────────────────────────────────────

void TileRenderer::blitRect(GLuint source, int sx, int sy, GLuint target, int dx, int dy, int width, int height) const {
    api_.bindFramebuffer(gl::kReadFramebuffer, readFbo_);
    api_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, source, 0);
    api_.bindFramebuffer(gl::kDrawFramebuffer, targetFbo_);
    api_.framebufferTexture2D(gl::kDrawFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, target, 0);
    glDisable(GL_SCISSOR_TEST);
    api_.blitFramebuffer(sx, sy, sx + width, sy + height, dx, dy, dx + width, dy + height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    api_.bindFramebuffer(gl::kReadFramebuffer, 0);
}

void TileRenderer::ensureWetTargets(int width, int height) {
    if (wetTex_.width == width && wetTex_.height == height) return;
    const auto make = [&](GLuint& texture, GLenum internalFormat, GLenum format, GLenum type) {
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        if (texture == 0) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        setNearestClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format, type, nullptr);
    };
    make(wetTex_.in, gl::kRgba16f, GL_RGBA, GL_FLOAT);
    make(wetTex_.out, gl::kRgba16f, GL_RGBA, GL_FLOAT);
    make(wetTex_.aux, gl::kRgba16f, GL_RGBA, GL_FLOAT);
    make(wetTex_.mask, gl::kR16f, GL_RED, GL_FLOAT);
    make(wetTex_.selection, gl::kR8, GL_RED, GL_UNSIGNED_BYTE);
    attach(wetTex_.selection);  // no selection yet: 1 everywhere (§4.1 neutral value)
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    wetTex_.width = width;
    wetTex_.height = height;
}

GLuint TileRenderer::maskTexture(const core::MaskRef& tile) {
    const auto it = maskTextures_.find(tile.get());
    if (it != maskTextures_.end()) return it->second.second;
    GLuint texture = 0;
    api_.activeTexture(gl::kTexture0 + kUnitUpload);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    setNearestClamp();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kR8), kTileSize, kTileSize, 0, GL_RED, GL_UNSIGNED_BYTE,
                 tile->bytes.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    maskTextures_.emplace(tile.get(), std::pair{tile, texture});
    return texture;
}

GLuint TileRenderer::selectionFor(int tx, int ty) {
    if (!strokeMask_) return selection_;  // no selection: ones
    const core::MaskRef tile = strokeMask_->tile(tx, ty);
    return tile ? maskTexture(tile) : zeroMask_;  // an existing selection forbids writing outside it
}

void TileRenderer::setSelection(const core::SelectionMask* mask) {
    if (mask) {
        strokeMask_ = *mask;
    } else {
        strokeMask_.reset();
    }
    // Uploaded tiles that no mask refers to any more go.
    std::set<const core::MaskTile*> referenced;
    if (strokeMask_) {
        for (const auto& [k, t] : strokeMask_->tiles()) referenced.insert(t.get());
    }
    for (auto it = maskTextures_.begin(); it != maskTextures_.end();) {
        const auto& [key, entry] = *it;
        if (!referenced.contains(key) && entry.first.use_count() == 1) {
            glDeleteTextures(1, &entry.second);
            it = maskTextures_.erase(it);
        } else {
            ++it;
        }
    }
}

void TileRenderer::assembleSelection(const PixelRect& region) {
    attach(wetTex_.selection);
    glViewport(0, 0, region.x1 - region.x0, region.y1 - region.y0);
    glDisable(GL_SCISSOR_TEST);
    const float neutral = strokeMask_ ? 0.0f : 1.0f;
    glClearColor(neutral, neutral, neutral, neutral);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!strokeMask_) return;
    for (int ty = region.y0 / kTileSize; ty <= (region.y1 - 1) / kTileSize; ++ty) {
        for (int tx = region.x0 / kTileSize; tx <= (region.x1 - 1) / kTileSize; ++tx) {
            const core::MaskRef tile = strokeMask_->tile(tx, ty);
            if (!tile) continue;
            const int x0 = std::max(region.x0, tx * kTileSize), x1 = std::min(region.x1, (tx + 1) * kTileSize);
            const int y0 = std::max(region.y0, ty * kTileSize), y1 = std::min(region.y1, (ty + 1) * kTileSize);
            blitRect(maskTexture(tile), x0 - tx * kTileSize, y0 - ty * kTileSize, wetTex_.selection, x0 - region.x0,
                     y0 - region.y0, x1 - x0, y1 - y0);
        }
    }
}

void TileRenderer::setFloating(const FloatingLayer* floating) { floating_ = floating; }

TileRenderer::Image TileRenderer::floatingContent(const LayerStack&, int tx, int ty) {
    Image base;
    if (const auto it = floating_->base.find({tx, ty}); it != floating_->base.end() && it->second) {
        base = {resident(*it->second), false};
    }
    // The floating part, shifted: output (x, y) shows floating (x − dx, y − dy).
    const int x0 = tx * kTileSize - floating_->dx;
    const int y0 = ty * kTileSize - floating_->dy;
    const auto floorDiv = [](int a) { return a >= 0 ? a / kTileSize : -((-a + kTileSize - 1) / kTileSize); };
    GLuint moved = 0;
    for (int sy = floorDiv(y0); sy <= floorDiv(y0 + kTileSize - 1); ++sy) {
        for (int sx = floorDiv(x0); sx <= floorDiv(x0 + kTileSize - 1); ++sx) {
            const auto it = floating_->floating.find({sx, sy});
            if (it == floating_->floating.end() || !it->second) continue;
            if (moved == 0) {
                moved = pool_->acquire(gl::kRgba16f);
                clearTexture(moved, 0.0f);
            }
            const int ix0 = std::max(x0, sx * kTileSize), ix1 = std::min(x0 + kTileSize, (sx + 1) * kTileSize);
            const int iy0 = std::max(y0, sy * kTileSize), iy1 = std::min(y0 + kTileSize, (sy + 1) * kTileSize);
            blitRect(resident(*it->second), ix0 - sx * kTileSize, iy0 - sy * kTileSize, moved, ix0 - x0, iy0 - y0, ix1 - ix0,
                     iy1 - iy0);
        }
    }
    return blendInto(base, moved ? Image{moved, true} : Image{}, 1.0f, 0);
}

// ── Filters (§9) ─────────────────────────────────────────────────────────────

void TileRenderer::ensureFxTargets(int width, int height) {
    if (fxTex_.width == width && fxTex_.height == height) return;
    for (GLuint* texture : {&fxTex_.in, &fxTex_.a, &fxTex_.b}) {
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        if (*texture == 0) glGenTextures(1, texture);
        glBindTexture(GL_TEXTURE_2D, *texture);
        setNearestClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kRgba16f), width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    fxTex_.width = width;
    fxTex_.height = height;
}

void TileRenderer::assembleFrom(const TileLookup& lookup, const PixelRect& region, GLuint target) {
    attach(target);
    glViewport(0, 0, region.x1 - region.x0, region.y1 - region.y0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int ty = region.y0 / kTileSize; ty <= (region.y1 - 1) / kTileSize; ++ty) {
        for (int tx = region.x0 / kTileSize; tx <= (region.x1 - 1) / kTileSize; ++tx) {
            const TileRef ref = lookup ? lookup(tx, ty) : nullptr;
            if (!ref) continue;
            const int x0 = std::max(region.x0, tx * kTileSize), x1 = std::min(region.x1, (tx + 1) * kTileSize);
            const int y0 = std::max(region.y0, ty * kTileSize), y1 = std::min(region.y1, (ty + 1) * kTileSize);
            blitRect(resident(*ref), x0 - tx * kTileSize, y0 - ty * kTileSize, target, x0 - region.x0, y0 - region.y0,
                     x1 - x0, y1 - y0);
        }
    }
}

GLuint TileRenderer::fxTile(const FxRequest& request, const TileLookup& committed, int tx, int ty) {
    const core::FxSettings& fx = request.settings;
    const int halo = core::fxHalo(fx);
    // The working region: the tile plus the halo, inside the document (§9: the edge pixel extends).
    const PixelRect region{std::max(0, tx * kTileSize - halo), std::max(0, ty * kTileSize - halo),
                           std::min(width_, (tx + 1) * kTileSize + halo), std::min(height_, (ty + 1) * kTileSize + halo)};
    const int w = region.x1 - region.x0;
    const int h = region.y1 - region.y0;
    ensureFxTargets(w, h);
    assembleFrom(committed, region, fxTex_.in);
    const auto pass = [&](gl::Program& program, GLuint input, GLuint second, GLuint output) {
        program.use();
        bindTexture(kUnitBase, input);
        if (second) bindTexture(kUnitLayer, second);
        attach(output);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    const auto blur = [&](GLuint input, GLuint scratch, GLuint output, int radius) {
        gaussian_->use();
        api_.uniform1i(gaussianU_.radiusPx, radius);
        api_.uniform2i(gaussianU_.direction, 1, 0);
        pass(*gaussian_, input, 0, scratch);
        api_.uniform2i(gaussianU_.direction, 0, 1);
        pass(*gaussian_, scratch, 0, output);
    };
    GLuint filtered = 0;
    switch (fx.kind) {
    case core::FxKind::Gaussian:
        blur(fxTex_.in, fxTex_.a, fxTex_.b, fx.radius);
        filtered = fxTex_.b;
        break;
    case core::FxKind::ToneCurve:
        toneCurve_->use();
        api_.uniform1i(fxU_.toneDiscrete, fx.curve.discrete ? 1 : 0);
        pass(*toneCurve_, fxTex_.in, curveTex_, fxTex_.b);
        filtered = fxTex_.b;
        break;
    case core::FxKind::Unsharp:
        blur(fxTex_.in, fxTex_.a, fxTex_.b, fx.radius);
        unsharp_->use();
        api_.uniform1f(fxU_.unsharpGain, static_cast<float>(fx.gain));
        api_.uniform1f(fxU_.unsharpThreshold, static_cast<float>(fx.threshold));
        pass(*unsharp_, fxTex_.in, fxTex_.b, fxTex_.a);
        filtered = fxTex_.a;
        break;
    case core::FxKind::Chromatic:
        chromatic_->use();
        api_.uniform2f(fxU_.chromaticShift, static_cast<float>(fx.shiftX), static_cast<float>(fx.shiftY));
        pass(*chromatic_, fxTex_.in, 0, fxTex_.b);
        filtered = fxTex_.b;
        break;
    case core::FxKind::AnimeLines: {
        GLuint input = fxTex_.in;
        if (fx.radius > 0) {
            blur(fxTex_.in, fxTex_.a, fxTex_.b, fx.radius);
            input = fxTex_.b;
        }
        animeLines_->use();
        api_.uniform1f(fxU_.animeBlack, static_cast<float>(fx.blackPoint));
        api_.uniform1f(fxU_.animeWhite, static_cast<float>(fx.whitePoint));
        api_.uniform1f(fxU_.animeMid, static_cast<float>(fx.mid));
        api_.uniform1f(fxU_.animeEdge, static_cast<float>(fx.edgeGain));
        pass(*animeLines_, input, 0, fxTex_.a);
        filtered = fxTex_.a;
        break;
    }
    }
    // Only the inner part goes on (§9: "В атлас переносить только внутреннюю часть результата").
    const GLuint inner = pool_->acquire(gl::kRgba16f);
    clearTexture(inner, 0.0f);
    const int x0 = tx * kTileSize, y0 = ty * kTileSize;
    const int ix1 = std::min(width_, x0 + kTileSize), iy1 = std::min(height_, y0 + kTileSize);
    blitRect(filtered, x0 - region.x0, y0 - region.y0, inner, 0, 0, ix1 - x0, iy1 - y0);

    // out = mix(original, filtered, selection·amount), then the lock rules (§9, §7).
    const TileRef original = committed ? committed(tx, ty) : nullptr;
    const GLuint originalTexture = original ? resident(*original) : zeroTile_;
    const bool toNew = request.target != request.source;
    GLuint result = pool_->acquire(gl::kRgba16f);
    fxMix_->use();
    api_.uniform1f(fxU_.mixAmount, static_cast<float>(fx.amount));
    api_.uniform1i(fxU_.mixToNew, toNew ? 1 : 0);
    bindTexture(kUnitBase, originalTexture);
    bindTexture(kUnitLayer, inner);
    bindTexture(kUnitSelection, selectionFor(tx, ty));
    runPass(result);
    pool_->release(gl::kRgba16f, inner);
    if (request.alphaLock && !toNew) {
        const GLuint locked = pool_->acquire(gl::kRgba16f);
        lockApply_->use();
        bindTexture(kUnitBase, originalTexture);
        bindTexture(kUnitLayer, result);
        runPass(locked);
        pool_->release(gl::kRgba16f, result);
        result = locked;
    }
    return result;
}

void TileRenderer::previewFx(const FxRequest& request, const TileLookup& committed) {
    cancelFx();
    const core::FxSettings fx = core::sanitize(request.settings);
    FxRequest clean = request;
    clean.settings = fx;
    if (fx.kind == core::FxKind::ToneCurve) {
        const auto table = core::curveTable(fx.curve);
        api_.activeTexture(gl::kTexture0 + kUnitUpload);
        if (curveTex_ == 0) glGenTextures(1, &curveTex_);
        glBindTexture(GL_TEXTURE_2D, curveTex_);
        setNearestClamp();
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kRgba32f), 256, 1, 0, GL_RGBA, GL_FLOAT, table.data());
    }
    fx_.active = true;
    fx_.target = request.target;
    fx_.replaces = request.target == request.source;
    ++fx_.epoch;
    for (const auto& [tx, ty] : request.tiles) {
        if (tx < 0 || ty < 0 || tx * kTileSize >= width_ || ty * kTileSize >= height_) continue;
        fx_.tiles[{tx, ty}] = fxTile(clean, committed, tx, ty);
    }
    api_.useProgram(0);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
}

std::vector<TileChange> TileRenderer::commitFx(const TileLookup& targetCommitted) {
    std::vector<TileChange> changes;
    if (!fx_.active) return changes;
    for (auto& [key, texture] : fx_.tiles) {
        const core::TileKey tileKey{fx_.target, key.first, key.second};
        const TileRef before = targetCommitted ? targetCommitted(key.first, key.second) : nullptr;
        TileRef after = finishTile(tileKey, texture, before);  // takes the texture
        texture = 0;
        if (before != after) changes.push_back({tileKey, before, std::move(after)});
    }
    fx_.tiles.clear();
    fx_.active = false;
    ++fx_.epoch;
    return changes;
}

void TileRenderer::cancelFx() {
    for (auto& [key, texture] : fx_.tiles) pool_->release(gl::kRgba16f, texture);
    fx_.tiles.clear();
    if (fx_.active) ++fx_.epoch;
    fx_.active = false;
}

void TileRenderer::assembleRegion(const PixelRect& region, GLuint target) {
    attach(target);
    glViewport(0, 0, region.x1 - region.x0, region.y1 - region.y0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // an empty tile inside the document is transparent
    glClear(GL_COLOR_BUFFER_BIT);
    for (int ty = region.y0 / kTileSize; ty <= (region.y1 - 1) / kTileSize; ++ty) {
        for (int tx = region.x0 / kTileSize; tx <= (region.x1 - 1) / kTileSize; ++tx) {
            GLuint source = 0;
            const auto it = tiles_.find(core::TileKey{setup_.layer, tx, ty});
            if (it != tiles_.end() && it->second.ready) {
                source = it->second.working;  // what the previous stamps published
            } else if (committed_) {
                if (const TileRef ref = committed_(tx, ty)) source = resident(*ref);
            }
            if (source == 0) continue;
            const int x0 = std::max(region.x0, tx * kTileSize), x1 = std::min(region.x1, (tx + 1) * kTileSize);
            const int y0 = std::max(region.y0, ty * kTileSize), y1 = std::min(region.y1, (ty + 1) * kTileSize);
            blitRect(source, x0 - tx * kTileSize, y0 - ty * kTileSize, target, x0 - region.x0, y0 - region.y0, x1 - x0, y1 - y0);
        }
    }
}

void TileRenderer::wetStamp(const core::Dab& dab) {
    if (!(dab.radius > 0.0)) return;
    const PixelRect box = dabRect(dab);  // B, clamped to the document
    if (box.x0 >= box.x1 || box.y0 >= box.y1) return;
    const double r = dab.radius;

    // Δ = c_k − c_{k−1}, at most 2r long; the first stamp has none.
    core::V delta{0.0, 0.0};
    if (wet_.started) {
        delta = dab.center - wet_.previous;
        const double length = std::hypot(delta.x, delta.y);
        if (length > 2.0 * r) delta = delta * (2.0 * r / length);
    }
    const double distance = std::hypot(delta.x, delta.y);
    const double bleed = std::clamp(static_cast<double>(setup_.bleed), 0.0, 1.0);
    const int blurRadius = std::clamp(setup_.blurRadius, 0, 32);

    // The read region: B plus the halo of the trail or of the blur, inside the document. Sizes are
    // rounded up to 32 px so the work textures keep their size from stamp to stamp.
    const int halo = setup_.stroke == StrokeKind::Blur ? blurRadius
                                                     : static_cast<int>(std::ceil(distance + bleed * r / 2.0 + 1.0));
    const auto span = [](int lo, int hi, int limit) {
        lo = std::max(0, lo);
        hi = std::min(limit, hi);
        const int size = std::min(limit, (hi - lo + 31) / 32 * 32);
        if (lo + size > limit) lo = limit - size;
        return std::pair{lo, lo + size};
    };
    const auto [rx0, rx1] = span(box.x0 - halo, box.x1 + halo, width_);
    const auto [ry0, ry1] = span(box.y0 - halo, box.y1 + halo, height_);
    const PixelRect region{rx0, ry0, rx1, ry1};
    const int w = rx1 - rx0;
    const int h = ry1 - ry0;
    ensureWetTargets(w, h);
    assembleRegion(region, wetTex_.in);

    // The §4.1 weight m·o·flow of this stamp on the region grid; the selection limits writing (§10.1)
    // while reading may reach past it.
    assembleSelection(region);
    attach(wetTex_.mask);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(box.x0 - rx0, box.y0 - ry0, box.x1 - box.x0, box.y1 - box.y0);
    glDisable(GL_BLEND);
    stamp_->use();
    api_.uniform2i(stampU_.originPx, rx0, ry0);
    setDabUniforms(dab);
    bindTexture(kUnitSelection, wetTex_.selection);
    bindTexture(kUnitPattern, activePattern_ ? activePattern_ : pattern_);
    bindTexture(kUnitTipMask, activeTip_ ? activeTip_ : tipMask_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    GLuint result = 0;
    if (setup_.stroke == StrokeKind::Blur) {
        // §5 Blur Brush: G_y(G_x(S)) with the §9.2 program, then mix(S, blur, w).
        gaussian_->use();
        api_.uniform1i(gaussianU_.radiusPx, blurRadius);
        api_.uniform2i(gaussianU_.direction, 1, 0);
        bindTexture(kUnitBase, wetTex_.in);
        attach(wetTex_.aux);
        glDisable(GL_SCISSOR_TEST);  // the rows the vertical pass needs
        glDrawArrays(GL_TRIANGLES, 0, 3);
        api_.uniform2i(gaussianU_.direction, 0, 1);
        bindTexture(kUnitBase, wetTex_.aux);
        attach(wetTex_.out);
        glEnable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        blurMix_->use();
        api_.uniform1f(blurStrengthU_, setup_.strength);
        bindTexture(kUnitBase, wetTex_.in);
        bindTexture(kUnitLayer, wetTex_.out);
        bindTexture(kUnitWeight, wetTex_.mask);
        attach(wetTex_.aux);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        result = wetTex_.aux;
    } else {
        // The surface under the centre, C, once per stamp; then H′ = mix(H, C, λ).
        reservoir_->use();
        api_.uniform2f(reservoirU_.centerPx, static_cast<float>(dab.center.x - rx0), static_cast<float>(dab.center.y - ry0));
        api_.uniform1f(reservoirU_.halfRadius, static_cast<float>(r / 2.0));
        bindTexture(kUnitBase, wetTex_.in);
        api_.bindSampler(kUnitBase, linearSampler_);
        attach(reservoirTex_);
        glViewport(0, 0, 1, 1);
        glDisable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<float, 4> c{};
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, c.data());
        const bool first = !wet_.started;
        if (first) {
            wet_.reservoir = c;
        } else {
            const double lambda = 1.0 - std::exp(-bleed * distance / std::max(0.25, r));
            for (std::size_t i = 0; i < 4; ++i) {
                wet_.reservoir[i] = static_cast<float>(wet_.reservoir[i] + (c[i] - wet_.reservoir[i]) * lambda);
            }
        }
        wet_.started = true;
        wet_.previous = dab.center;
        if (first && setup_.stroke == StrokeKind::Smudge) {  // §5: the finger's first stamp only picks up
            api_.bindSampler(kUnitBase, 0);
            return;
        }
        wetProgram_->use();
        const auto& ink = setup_.inkPma;
        const auto& h0 = wet_.reservoir;
        api_.uniform2f(wetU_.deltaPx, static_cast<float>(delta.x), static_cast<float>(delta.y));
        api_.uniform4f(wetU_.reservoir, h0[0], h0[1], h0[2], h0[3]);
        api_.uniform4f(wetU_.freshInk, ink[0], ink[1], ink[2], ink[3]);
        api_.uniform1f(wetU_.radius, static_cast<float>(r));
        api_.uniform1f(wetU_.blending, setup_.blending);
        api_.uniform1f(wetU_.dilution, setup_.dilution);
        api_.uniform1f(wetU_.bleed, setup_.bleed);
        api_.uniform1f(wetU_.strength, setup_.strength);
        api_.uniform1i(wetU_.watercolor, setup_.stroke == StrokeKind::Watercolor ? 1 : 0);
        api_.uniform1i(wetU_.emitSource, 0);
        bindTexture(kUnitWeight, wetTex_.mask);
        attach(wetTex_.out);
        glViewport(0, 0, w, h);
        glEnable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        api_.bindSampler(kUnitBase, 0);
        result = wetTex_.out;
    }
    if (setup_.alphaLock) {
        // §7: the candidate's straight colour on the old alpha.
        const GLuint locked = result == wetTex_.out ? wetTex_.aux : wetTex_.out;
        lockApply_->use();
        bindTexture(kUnitBase, wetTex_.in);
        bindTexture(kUnitLayer, result);
        attach(locked);
        glViewport(0, 0, w, h);
        glEnable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        result = locked;
    }

    // Publish B into the working tiles: the next stamp reads it.
    for (int ty = box.y0 / kTileSize; ty <= (box.y1 - 1) / kTileSize; ++ty) {
        for (int tx = box.x0 / kTileSize; tx <= (box.x1 - 1) / kTileSize; ++tx) {
            StrokeTile& tile = prepareTile(core::TileKey{setup_.layer, tx, ty}, false);
            tile.touched = true;
            const int x0 = std::max(box.x0, tx * kTileSize), x1 = std::min(box.x1, (tx + 1) * kTileSize);
            const int y0 = std::max(box.y0, ty * kTileSize), y1 = std::min(box.y1, (ty + 1) * kTileSize);
            blitRect(result, x0 - rx0, y0 - ry0, tile.working, x0 - tx * kTileSize, y0 - ty * kTileSize, x1 - x0, y1 - y0);
        }
    }
}

void TileRenderer::setTail(std::span<const core::Dab> dabs) {
    if (!strokeActive_ || wet()) return;  // wet strokes show only what is final
    for (auto& entry : tiles_) {
        if (entry.second.tail) releasePreview(entry.second);
    }
    stampBatch(dabs, true);
}

void TileRenderer::replay(std::span<const core::Dab> dabs) {
    if (!strokeActive_) return;
    ++strokeEpoch_;
    wet_ = {};
    for (auto& entry : tiles_) {
        StrokeTile& tile = entry.second;
        releasePreview(tile);
        tile.touched = false;
        if (!tile.ready) continue;
        if (setup_.maxMode) {
            clearTexture(tile.weight, 0.0f);
        } else if (tile.before) {
            copyTexture(resident(*tile.before), tile.working);
        } else {
            clearTexture(tile.working, 0.0f);
        }
    }
    stampBatch(dabs, false);
    std::erase_if(tiles_, [this](auto& entry) {
        if (entry.second.touched) return false;
        releaseTile(entry.second);
        return true;
    });
}

std::vector<TileChange> TileRenderer::commitStroke() {
    std::vector<TileChange> changes;
    if (!strokeActive_) return changes;
    for (auto& [key, tile] : tiles_) {
        releasePreview(tile);
        if (!tile.touched) {
            releaseTile(tile);
            continue;
        }
        GLuint result = 0;
        if (setup_.maxMode) {
            result = pool_->acquire(gl::kRgba16f);
            const GLuint base = tile.before ? resident(*tile.before) : zeroTile_;
            attach(result);
            glViewport(0, 0, kTileSize, kTileSize);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            resolve_->use();
            api_.uniform1i(resolveU_.hasBase, tile.before ? 1 : 0);
            bindTexture(kUnitBase, base);
            bindTexture(kUnitWeight, tile.weight);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        } else {
            result = tile.working;
            tile.working = 0;  // ownership moves to the new version
        }
        TileRef after = finishTile(key, result, tile.before);
        changes.push_back(TileChange{key, tile.before, std::move(after)});
        releaseTile(tile);
    }
    tiles_.clear();
    strokeActive_ = false;
    committed_ = nullptr;
    ++strokeEpoch_;
    api_.useProgram(0);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    return changes;
}

void TileRenderer::cancelStroke() {
    for (auto& entry : tiles_) releaseTile(entry.second);
    tiles_.clear();
    strokeActive_ = false;
    committed_ = nullptr;
    ++strokeEpoch_;
}

// ── Presentation ─────────────────────────────────────────────────────────────

// ── Composition (§7) ─────────────────────────────────────────────────────────

void TileRenderer::runPass(GLuint target) const {
    attach(target);
    glViewport(0, 0, kTileSize, kTileSize);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void TileRenderer::drop(Image& image) {
    if (image.owned && image.texture != 0) pool_->release(gl::kRgba16f, image.texture);
    image = {};
}

TileRenderer::Image TileRenderer::blendInto(Image dst, Image src, float opacity, int mode) {
    if (src.texture == 0 || !(opacity > 0.0f)) {
        drop(src);
        return dst;
    }
    // Over nothing every mode gives the source scaled by the opacity: at full opacity, the source.
    if (dst.texture == 0 && opacity >= 1.0f) return src;
    const GLuint out = pool_->acquire(gl::kRgba16f);
    composite_->use();
    api_.uniform1f(compositeU_.opacity, opacity);
    api_.uniform1i(compositeU_.mode, mode);
    bindTexture(kUnitBase, dst.texture != 0 ? dst.texture : zeroTile_);
    bindTexture(kUnitLayer, src.texture);
    runPass(out);
    drop(dst);
    drop(src);
    return {out, true};
}

TileRenderer::Image TileRenderer::unitCoverage(const Image& base) {
    const GLuint out = pool_->acquire(gl::kRgba16f);
    unitCoverage_->use();
    bindTexture(kUnitLayer, base.texture);
    runPass(out);
    return {out, true};
}

TileRenderer::Image TileRenderer::clipToSilhouette(Image chain, const Image& base, float opacity) {
    const GLuint out = pool_->acquire(gl::kRgba16f);
    clip_->use();
    api_.uniform1f(clipOpacityU_, opacity);
    bindTexture(kUnitBase, chain.texture);
    bindTexture(kUnitLayer, base.texture);
    runPass(out);
    drop(chain);
    return {out, true};
}

TileRenderer::Image TileRenderer::rasterContent(const LayerStack& stack, std::uint64_t layer, int tx, int ty) {
    if (floating_ && layer == floating_->layer) return floatingContent(stack, tx, ty);
    if (fx_.active && layer == fx_.target) {
        if (const auto it = fx_.tiles.find({tx, ty}); it != fx_.tiles.end()) return {it->second, false};
        if (!fx_.replaces) return {};
    }
    if (strokeActive_ && layer == setup_.layer) {
        const auto it = tiles_.find(core::TileKey{layer, tx, ty});
        if (it != tiles_.end() && it->second.ready) {
            // The live stroke enters the composite in its resolved state, exactly as it will commit.
            StrokeTile& tile = it->second;
            if (!setup_.maxMode) return {tile.tail ? tile.previewWorking : tile.working, false};
            const GLuint base = tile.before ? resident(*tile.before) : zeroTile_;
            const GLuint out = pool_->acquire(gl::kRgba16f);
            resolve_->use();
            api_.uniform1i(resolveU_.hasBase, tile.before ? 1 : 0);
            bindTexture(kUnitBase, base);
            bindTexture(kUnitWeight, tile.tail ? tile.previewWeight : tile.weight);
            runPass(out);
            return {out, true};
        }
    }
    if (stack.tiles) {
        if (const TileRef ref = stack.tiles(layer, tx, ty)) return {resident(*ref), false};
    }
    return {};
}

TileRenderer::Image TileRenderer::content(const LayerStack& stack, const core::LayerNode& node, int tx, int ty) {
    return node.kind == core::NodeKind::Group ? composeChildren(stack, node.id, tx, ty)
                                              : rasterContent(stack, node.id, tx, ty);
}

TileRenderer::Image TileRenderer::composeChildren(const LayerStack& stack, std::uint64_t parent, int tx, int ty) {
    const core::LayerTree& tree = *stack.tree;
    const std::vector<std::uint64_t>& list = tree.children(parent);
    Image scene;
    for (std::size_t i = 0; i < list.size();) {
        const core::LayerNode& node = *tree.find(list[i]);
        std::size_t j = i + 1;
        while (j < list.size() && tree.find(list[j])->props.clipToBase) ++j;
        if (node.props.clipToBase) {  // orphan clip: nothing below to clip to
            ++i;
            continue;
        }
        if (!node.props.visible) {  // a hidden base hides its chain
            i = j;
            continue;
        }
        Image base = content(stack, node, tx, ty);
        if (base.texture == 0) {  // so does an empty one
            i = j;
            continue;
        }
        const auto opacity = static_cast<float>(node.props.opacity);
        if (j == i + 1) {
            scene = blendInto(scene, base, opacity, node.props.blendId);
            i = j;
            continue;
        }
        // Clip chain: the base colour at unit coverage, the clipped layers on it with their own
        // opacity and mode, the base silhouette, then the chain over the scene in the base mode.
        Image chain = unitCoverage(base);
        for (std::size_t k = i + 1; k < j; ++k) {
            const core::LayerNode& top = *tree.find(list[k]);
            if (!top.props.visible) continue;
            chain = blendInto(chain, content(stack, top, tx, ty), static_cast<float>(top.props.opacity),
                              top.props.blendId);
        }
        Image cut = clipToSilhouette(chain, base, opacity);
        drop(base);
        scene = blendInto(scene, cut, 1.0f, node.props.blendId);
        i = j;
    }
    return scene;
}

void TileRenderer::signatureOf(const LayerStack& stack, int tx, int ty, std::vector<std::uint64_t>& out) {
    out.clear();
    out.push_back(treeHash_);
    for (const std::uint64_t layer : rasters_) {
        if (floating_ && layer == floating_->layer) {
            out.push_back(kFloatToken | floating_->epoch);
            continue;
        }
        if (fx_.active && layer == fx_.target) {
            out.push_back(kFxToken | fx_.epoch);
            continue;
        }
        if (strokeActive_ && layer == setup_.layer) {
            const auto it = tiles_.find(core::TileKey{layer, tx, ty});
            if (it != tiles_.end() && it->second.ready) {
                out.push_back(kStrokeToken | strokeEpoch_);
                continue;
            }
        }
        const TileRef ref = stack.tiles ? stack.tiles(layer, tx, ty) : nullptr;
        out.push_back(ref ? ref->serial() : 0);
    }
}

TileRenderer::Image TileRenderer::displayTile(const LayerStack& stack, int tx, int ty) {
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tx)) << 32) |
                              static_cast<std::uint32_t>(ty);
    signatureOf(stack, tx, ty, signature_);
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second.signature == signature_) {
        it->second.lastUse = frame_;
        return {it->second.texture, false};
    }
    Image image = composeChildren(stack, 0, tx, ty);
    if (it != cache_.end()) {
        pool_->release(gl::kRgba16f, it->second.texture);
        cache_.erase(it);
    }
    if (!image.owned) return image;  // nothing, or one layer shown as it is: nothing to keep
    if (cache_.size() >= kCacheTiles) {
        const auto victim = std::min_element(cache_.begin(), cache_.end(), [](const auto& a, const auto& b) {
            return a.second.lastUse < b.second.lastUse;
        });
        if (victim->second.lastUse == frame_) return image;  // every cached tile is on screen
        pool_->release(gl::kRgba16f, victim->second.texture);
        cache_.erase(victim);
    }
    cache_.emplace(key, CacheEntry{image.texture, signature_, frame_});
    return {image.texture, false};
}

void TileRenderer::clearCache() {
    for (auto& [key, entry] : cache_) pool_->release(gl::kRgba16f, entry.texture);
    cache_.clear();
}

// ── Presentation ─────────────────────────────────────────────────────────────

void TileRenderer::drawDocument(const DocumentFrame& frame, const LayerStack& stack) {
    const int viewportWidth = frame.viewportX1 - frame.viewportX0;
    const int viewportHeight = frame.viewportY1 - frame.viewportY0;
    if (!stack.tree || viewportWidth <= 0 || viewportHeight <= 0 || frame.clientWidth <= 0 || frame.clientHeight <= 0 ||
        !(frame.pixelsPerDocPx > 0)) {
        return;
    }

    // Visible part of the document, in document pixels.
    const double k = frame.pixelsPerDocPx;
    const double x0 = std::max(0.0, std::floor((frame.viewportX0 - frame.originX) / k));
    const double x1 = std::min(static_cast<double>(width_), std::ceil((frame.viewportX1 - frame.originX) / k));
    const double y0 = std::max(0.0, std::floor((frame.viewportY0 - frame.originY) / k));
    const double y1 = std::min(static_cast<double>(height_), std::ceil((frame.viewportY1 - frame.originY) / k));
    if (x0 >= x1 || y0 >= y1) return;
    const int tx0 = static_cast<int>(x0) / kTileSize;
    const int tx1 = (static_cast<int>(x1) - 1) / kTileSize;
    const int ty0 = static_cast<int>(y0) / kTileSize;
    const int ty1 = (static_cast<int>(y1) - 1) / kTileSize;

    // The render state of the tree: everything that changes pixels, nothing that does not (names,
    // the active layer, locks).
    ++frame_;
    rasters_ = stack.tree->rasters();
    treeHash_ = 0xCBF29CE484222325ull;
    for (const std::uint64_t id : stack.tree->preorder()) {
        const core::LayerNode& n = *stack.tree->find(id);
        hashInto(treeHash_, n.id);
        hashInto(treeHash_, n.parent);
        hashInto(treeHash_, static_cast<std::uint64_t>(n.kind));
        hashInto(treeHash_, (n.props.visible ? 1u : 0u) | (n.props.clipToBase ? 2u : 0u));
        hashInto(treeHash_, std::bit_cast<std::uint64_t>(n.props.opacity));
        hashInto(treeHash_, static_cast<std::uint64_t>(n.props.blendId));
    }

    // Minified: every screen pixel averages the document pixels under it (thin lines stay whole).
    if (k < 1.0) {
        drawMinified(frame, stack, tx0, tx1, ty0, ty1);
        return;
    }
    // Magnified (and 1:1) pixels stay crisp.
    const GLuint sampler = nearestSampler_;
    const double toNdcX = 2.0 / frame.clientWidth;
    const double toNdcY = 2.0 / frame.clientHeight;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            Image tile = displayTile(stack, tx, ty);
            if (tile.texture == 0) continue;  // transparent: the paper shows through
            const int dx0 = tx * kTileSize;
            const int dy0 = ty * kTileSize;
            const int dx1 = std::min(dx0 + kTileSize, width_);
            const int dy1 = std::min(dy0 + kTileSize, height_);
            const double sx0 = frame.originX + dx0 * k;
            const double sx1 = frame.originX + dx1 * k;
            const double sy0 = frame.originY + dy0 * k;
            const double sy1 = frame.originY + dy1 * k;
            // Composition used offscreen targets: back to the window for this tile.
            api_.bindFramebuffer(gl::kFramebuffer, 0);
            glViewport(0, 0, frame.clientWidth, frame.clientHeight);
            glEnable(GL_SCISSOR_TEST);
            glScissor(frame.viewportX0, frame.clientHeight - frame.viewportY1, viewportWidth, viewportHeight);
            glDisable(GL_BLEND);
            display_->use();
            api_.uniform3f(displayU_.paper, frame.paper[0], frame.paper[1], frame.paper[2]);
            api_.uniform1f(displayU_.opacity, 1.0f);
            api_.uniform4f(displayU_.rectNdc, static_cast<float>(sx0 * toNdcX - 1.0), static_cast<float>(1.0 - sy0 * toNdcY),
                           static_cast<float>(sx1 * toNdcX - 1.0), static_cast<float>(1.0 - sy1 * toNdcY));
            api_.uniform4f(displayU_.uvRect, 0.0f, 0.0f, static_cast<float>(dx1 - dx0) / kTileSize,
                           static_cast<float>(dy1 - dy0) / kTileSize);
            api_.bindSampler(kUnitLayer, sampler);
            bindTexture(kUnitLayer, tile.texture);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            api_.bindSampler(kUnitLayer, 0);
            drop(tile);
        }
    }
    api_.useProgram(0);
}

void TileRenderer::drawMinified(const DocumentFrame& frame, const LayerStack& stack, int tx0, int tx1, int ty0, int ty1) {
    const int viewportWidth = frame.viewportX1 - frame.viewportX0;
    const int viewportHeight = frame.viewportY1 - frame.viewportY0;
    const double k = frame.pixelsPerDocPx;
    // The accumulator follows the client size.
    if (accumWidth_ != frame.clientWidth || accumHeight_ != frame.clientHeight || accumTex_ == 0) {
        if (accumTex_ == 0) glGenTextures(1, &accumTex_);
        if (accumFbo_ == 0) api_.genFramebuffers(1, &accumFbo_);
        api_.activeTexture(gl::kTexture0 + kUnitLayer);
        glBindTexture(GL_TEXTURE_2D, accumTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kRgba16f), frame.clientWidth, frame.clientHeight, 0, GL_RGBA,
                     GL_FLOAT, nullptr);
        api_.bindFramebuffer(gl::kFramebuffer, accumFbo_);
        api_.framebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, accumTex_, 0);
        accumWidth_ = frame.clientWidth;
        accumHeight_ = frame.clientHeight;
    }
    const double toNdcX = 2.0 / frame.clientWidth;
    const double toNdcY = 2.0 / frame.clientHeight;
    const auto beginAccumulation = [&] {
        api_.bindFramebuffer(gl::kFramebuffer, accumFbo_);
        glViewport(0, 0, frame.clientWidth, frame.clientHeight);
        glEnable(GL_SCISSOR_TEST);
        glScissor(frame.viewportX0, frame.clientHeight - frame.viewportY1, viewportWidth, viewportHeight);
    };
    beginAccumulation();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    bool any = false;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            Image tile = displayTile(stack, tx, ty);  // may compose through offscreen targets
            if (tile.texture == 0) continue;  // transparent: adds nothing
            any = true;
            const int dx0 = tx * kTileSize;
            const int dy0 = ty * kTileSize;
            // Every screen pixel whose footprint touches the tile: its rectangle grown by one pixel.
            const double sx0 = std::floor(frame.originX + dx0 * k) - 1.0;
            const double sx1 = std::ceil(frame.originX + (dx0 + kTileSize) * k) + 1.0;
            const double sy0 = std::floor(frame.originY + dy0 * k) - 1.0;
            const double sy1 = std::ceil(frame.originY + (dy0 + kTileSize) * k) + 1.0;
            beginAccumulation();
            glEnable(GL_BLEND);
            api_.blendEquation(gl::kFuncAdd);
            api_.blendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            boxDisplay_->use();
            api_.uniform4f(boxU_.rectNdc, static_cast<float>(sx0 * toNdcX - 1.0), static_cast<float>(1.0 - sy0 * toNdcY),
                           static_cast<float>(sx1 * toNdcX - 1.0), static_cast<float>(1.0 - sy1 * toNdcY));
            api_.uniform4f(boxU_.uvRect, 0.0f, 0.0f, 1.0f, 1.0f);
            api_.uniform2f(boxU_.originPx, static_cast<float>(frame.originX), static_cast<float>(frame.originY));
            api_.uniform1f(boxU_.clientHeight, static_cast<float>(frame.clientHeight));
            api_.uniform1f(boxU_.docPerPx, static_cast<float>(1.0 / k));
            api_.uniform2f(boxU_.tileOrigin, static_cast<float>(dx0), static_cast<float>(dy0));
            api_.bindSampler(kUnitLayer, nearestSampler_);
            bindTexture(kUnitLayer, tile.texture);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            api_.bindSampler(kUnitLayer, 0);
            glDisable(GL_BLEND);
            drop(tile);
        }
    }
    // Over the paper rectangle (rounded as the shell paints it), one encode.
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    glViewport(0, 0, frame.clientWidth, frame.clientHeight);
    const int px0 = std::max(frame.viewportX0, static_cast<int>(std::lround(frame.originX)));
    const int py0 = std::max(frame.viewportY0, static_cast<int>(std::lround(frame.originY)));
    const int px1 = std::min(frame.viewportX1, static_cast<int>(std::lround(frame.originX + width_ * k)));
    const int py1 = std::min(frame.viewportY1, static_cast<int>(std::lround(frame.originY + height_ * k)));
    if (any && px1 > px0 && py1 > py0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(px0, frame.clientHeight - py1, px1 - px0, py1 - py0);
        glDisable(GL_BLEND);
        boxResolve_->use();
        api_.uniform4f(boxResolveU_.rectNdc, -1.0f, 1.0f, 1.0f, -1.0f);
        api_.uniform4f(boxResolveU_.uvRect, 0.0f, 0.0f, 1.0f, 1.0f);
        api_.uniform3f(boxResolveU_.paper, frame.paper[0], frame.paper[1], frame.paper[2]);
        api_.bindSampler(kUnitLayer, nearestSampler_);
        bindTexture(kUnitLayer, accumTex_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        bindTexture(kUnitLayer, 0);  // never sampled while it is the next frame's render target
        api_.bindSampler(kUnitLayer, 0);
    }
    api_.useProgram(0);
}

void TileRenderer::drawSelection(const DocumentFrame& frame, const core::SelectionMask& mask, int dx, int dy) {
    const int viewportWidth = frame.viewportX1 - frame.viewportX0;
    const int viewportHeight = frame.viewportY1 - frame.viewportY0;
    if (viewportWidth <= 0 || viewportHeight <= 0 || mask.empty() || !(frame.pixelsPerDocPx > 0)) return;
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    glViewport(0, 0, frame.clientWidth, frame.clientHeight);
    glEnable(GL_SCISSOR_TEST);
    glScissor(frame.viewportX0, frame.clientHeight - frame.viewportY1, viewportWidth, viewportHeight);
    glDisable(GL_BLEND);
    const double k = frame.pixelsPerDocPx;
    const double toNdcX = 2.0 / frame.clientWidth;
    const double toNdcY = 2.0 / frame.clientHeight;
    const auto texture = [&](int tx, int ty) {
        const core::MaskRef t = mask.tile(tx, ty);
        return t ? maskTexture(t) : zeroMask_;
    };
    for (const auto& [key, tile] : mask.tiles()) {
        const auto [tx, ty] = key;
        // The tile's document area, shifted, on screen.
        const double ox = frame.originX + (tx * kTileSize + dx) * k;
        const double oy = frame.originY + (ty * kTileSize + dy) * k;
        const double ex = ox + kTileSize * k;
        const double ey = oy + kTileSize * k;
        if (ex < frame.viewportX0 || ox > frame.viewportX1 || ey < frame.viewportY0 || oy > frame.viewportY1) continue;
        bindTexture(kUnitSelection, texture(tx, ty));
        bindTexture(kUnitPattern, texture(tx - 1, ty));
        bindTexture(kUnitTipMask, texture(tx + 1, ty));
        bindTexture(kUnitWeight, texture(tx, ty - 1));
        bindTexture(kUnitBase, texture(tx, ty + 1));
        selectionOutline_->use();
        api_.uniform4f(outlineU_.rectNdc, static_cast<float>(ox * toNdcX - 1.0), static_cast<float>(1.0 - oy * toNdcY),
                       static_cast<float>(ex * toNdcX - 1.0), static_cast<float>(1.0 - ey * toNdcY));
        api_.uniform4f(outlineU_.uvRect, 0.0f, 0.0f, 1.0f, 1.0f);
        api_.uniform2f(outlineU_.tileOrigin, static_cast<float>(ox), static_cast<float>(oy));
        api_.uniform1f(outlineU_.scale, static_cast<float>(k));
        api_.uniform1f(outlineU_.screenHeight, static_cast<float>(frame.clientHeight));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    api_.useProgram(0);
}

void TileRenderer::drawBrushRing(const DocumentFrame& frame, double centerX, double centerY, double radiusPx) {
    const int viewportWidth = frame.viewportX1 - frame.viewportX0;
    const int viewportHeight = frame.viewportY1 - frame.viewportY0;
    if (viewportWidth <= 0 || viewportHeight <= 0 || !(radiusPx > 0)) return;

    api_.bindFramebuffer(gl::kFramebuffer, 0);
    glViewport(0, 0, frame.clientWidth, frame.clientHeight);
    glEnable(GL_SCISSOR_TEST);
    glScissor(frame.viewportX0, frame.clientHeight - frame.viewportY1, viewportWidth, viewportHeight);
    glEnable(GL_BLEND);
    api_.blendEquation(gl::kFuncAdd);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const double extent = radiusPx + 3.0;
    const double toNdcX = 2.0 / frame.clientWidth;
    const double toNdcY = 2.0 / frame.clientHeight;
    ring_->use();
    api_.uniform4f(ringU_.rectNdc, static_cast<float>((centerX - extent) * toNdcX - 1.0),
                   static_cast<float>(1.0 - (centerY - extent) * toNdcY),
                   static_cast<float>((centerX + extent) * toNdcX - 1.0),
                   static_cast<float>(1.0 - (centerY + extent) * toNdcY));
    api_.uniform4f(ringU_.uvRect, 0.0f, 0.0f, 1.0f, 1.0f);
    api_.uniform2f(ringU_.center, static_cast<float>(centerX), static_cast<float>(frame.clientHeight - centerY));
    api_.uniform1f(ringU_.radius, static_cast<float>(radiusPx));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    api_.useProgram(0);
}

std::vector<float> TileRenderer::readComposite(const LayerStack& stack, int tx, int ty) {
    std::vector<float> pixels(static_cast<std::size_t>(kTileSize) * kTileSize * 4, 0.0f);
    if (!stack.tree) return pixels;
    Image image = composeChildren(stack, 0, tx, ty);
    if (image.texture != 0) {
        api_.bindFramebuffer(gl::kReadFramebuffer, readFbo_);
        api_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, image.texture, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kTileSize, kTileSize, GL_RGBA, GL_FLOAT, pixels.data());
        api_.bindFramebuffer(gl::kReadFramebuffer, 0);
    }
    drop(image);
    api_.useProgram(0);
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    return pixels;
}

std::vector<float> TileRenderer::readResident(const TileVersion& version) {
    const GLuint texture = resident(version);
    std::vector<float> pixels(static_cast<std::size_t>(kTileSize) * kTileSize * 4);
    api_.bindFramebuffer(gl::kReadFramebuffer, readFbo_);
    api_.framebufferTexture2D(gl::kReadFramebuffer, gl::kColorAttachment0, GL_TEXTURE_2D, texture, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, kTileSize, kTileSize, GL_RGBA, GL_FLOAT, pixels.data());
    api_.bindFramebuffer(gl::kReadFramebuffer, 0);
    return pixels;
}

}  // namespace chroma::render

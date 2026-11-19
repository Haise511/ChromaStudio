#include "ui/UiRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace chroma::ui {
namespace {

constexpr std::size_t kMaxCachedTexts = 768;

// A quad in NDC with interpolated local coordinates (pixels or 0..1, per program).
constexpr const char* kVertex = R"glsl(#version 330 core
uniform vec4 rectNdc;
uniform vec4 localRect;
out vec2 local;
void main() {
    vec2 c=vec2(gl_VertexID&1,gl_VertexID>>1);
    gl_Position=vec4(mix(rectNdc.x,rectNdc.z,c.x),mix(rectNdc.y,rectNdc.w,c.y),0.0,1.0);
    local=vec2(mix(localRect.x,localRect.z,c.x),mix(localRect.y,localRect.w,c.y));
}
)glsl";

// Signed-distance primitives, all in pixels: 0 rounded box (p0 = half size, radius, stroke width or 0),
// 1 capsule (p0 = a, b; p1.x = half width), 2 disc / ring (p0.x = radius, p0.y = stroke width or 0).
constexpr const char* kShapeFragment = R"glsl(#version 330 core
uniform int kind;
uniform vec4 color;
uniform vec4 p0;
uniform vec4 p1;
in vec2 local;
layout(location=0) out vec4 pixel;
float box(vec2 p,vec2 b,float r) { vec2 q=abs(p)-b+r; return length(max(q,0.0))+min(max(q.x,q.y),0.0)-r; }
void main() {
    float d;
    if(kind==0) {
        d=box(local,p0.xy,p0.z);
        if(p0.w>0.0) d=abs(d+p0.w*0.5)-p0.w*0.5;
    } else if(kind==1) {
        vec2 pa=local-p0.xy, ba=p0.zw-p0.xy;
        float h=clamp(dot(pa,ba)/max(dot(ba,ba),1e-6),0.0,1.0);
        d=length(pa-ba*h)-p1.x;
    } else {
        d=length(local)-p0.x;
        if(p0.y>0.0) d=abs(d+p0.y*0.5)-p0.y*0.5;
    }
    float a=clamp(0.5-d,0.0,1.0);
    if(a<=0.0) discard;
    pixel=color*a;
}
)glsl";

constexpr const char* kTextFragment = R"glsl(#version 330 core
uniform sampler2D coverage;
uniform vec4 color;
in vec2 local;
layout(location=0) out vec4 pixel;
void main() { pixel=color*texture(coverage,local).r; }
)glsl";

// §11.1: HSV in encoded sRGB through six sectors.
constexpr const char* kHsv = R"glsl(
vec3 hsv2rgb(float h,float s,float v) {
    float h6=fract(h)*6.0, c=v*s, x=c*(1.0-abs(mod(h6,2.0)-1.0)), m=v-c;
    int k=int(floor(h6));
    vec3 rgb=k==0?vec3(c,x,0):k==1?vec3(x,c,0):k==2?vec3(0,c,x):k==3?vec3(0,x,c):k==4?vec3(x,0,c):vec3(c,0,x);
    return rgb+m;
}
)glsl";

// Ring of hues: H = mod(atan2(−dy, dx) / 2π, 1), red on the right, counter-clockwise.
constexpr const char* kHueFragment = R"glsl(
uniform vec2 radii;
in vec2 local;
layout(location=0) out vec4 pixel;
void main() {
    float r=length(local);
    float a=clamp(0.5-max(radii.x-r,r-radii.y),0.0,1.0);
    if(a<=0.0) discard;
    float h=fract(atan(-local.y,local.x)/6.283185307179586);
    pixel=vec4(hsv2rgb(h,1.0,1.0),1.0)*a;
}
)glsl";

// S grows to the right, V upwards.
constexpr const char* kSvFragment = R"glsl(
uniform float hue;
in vec2 local;
layout(location=0) out vec4 pixel;
void main() { pixel=vec4(hsv2rgb(hue,clamp(local.x,0.0,1.0),1.0-clamp(local.y,0.0,1.0)),1.0); }
)glsl";

constexpr const char* kCheckerFragment = R"glsl(#version 330 core
uniform float cell;
in vec2 local;
layout(location=0) out vec4 pixel;
void main() {
    vec2 c=floor(local/max(cell,1.0));
    float light=mod(c.x+c.y,2.0);
    pixel=vec4(vec3(mix(0.62,0.82,light)),1.0);
}
)glsl";

constexpr const char* kImageFragment = R"glsl(#version 330 core
uniform sampler2D picture;
in vec2 local;
layout(location=0) out vec4 pixel;
void main() { vec4 c=texture(picture,local); pixel=vec4(c.rgb*c.a,c.a); }
)glsl";

std::string withHsv(const char* body) { return std::string("#version 330 core\n") + kHsv + body; }

}  // namespace

// ── UiImage ──────────────────────────────────────────────────────────────────

UiImage::UiImage(const gl::Context& context) : lifetime_(context.lifetime()) {
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl::kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl::kClampToEdge);
}

UiImage::~UiImage() {
    if (texture_ && !lifetime_.expired()) glDeleteTextures(1, &texture_);
}

void UiImage::update(int width, int height, const std::uint8_t* rgba) {
    width_ = width;
    height_ = height;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// ── UiRenderer ───────────────────────────────────────────────────────────────

UiRenderer::UiRenderer(const gl::Context& context) : api_(context.api()), lifetime_(context.lifetime()) {
    shape_ = std::make_unique<gl::Program>(context, "ui shape", kVertex, kShapeFragment);
    text_ = std::make_unique<gl::Program>(context, "ui text", kVertex, kTextFragment);
    const std::string hue = withHsv(kHueFragment);
    const std::string sv = withHsv(kSvFragment);
    hue_ = std::make_unique<gl::Program>(context, "ui hue ring", kVertex, hue.c_str());
    sv_ = std::make_unique<gl::Program>(context, "ui sv square", kVertex, sv.c_str());
    checker_ = std::make_unique<gl::Program>(context, "ui checker", kVertex, kCheckerFragment);
    image_ = std::make_unique<gl::Program>(context, "ui image", kVertex, kImageFragment);

    shapeU_ = {shape_->uniform("rectNdc"), shape_->uniform("localRect"), shape_->uniform("kind"),
               shape_->uniform("color"),   shape_->uniform("p0"),        shape_->uniform("p1")};
    textU_ = {text_->uniform("rectNdc"), text_->uniform("localRect"), text_->uniform("color")};
    hueU_ = {hue_->uniform("rectNdc"), hue_->uniform("localRect"), hue_->uniform("radii")};
    svU_ = {sv_->uniform("rectNdc"), sv_->uniform("localRect"), sv_->uniform("hue")};
    checkerU_ = {checker_->uniform("rectNdc"), checker_->uniform("localRect"), checker_->uniform("cell")};
    imageU_ = {image_->uniform("rectNdc"), image_->uniform("localRect")};
    text_->use();
    api_.uniform1i(text_->uniform("coverage"), 0);
    image_->use();
    api_.uniform1i(image_->uniform("picture"), 0);
    api_.useProgram(0);

    dc_ = CreateCompatibleDC(nullptr);
}

UiRenderer::~UiRenderer() {
    if (!lifetime_.expired()) {
        for (auto& [key, entry] : textCache_) glDeleteTextures(1, &entry.texture);
    }
    for (auto& [key, f] : fonts_) DeleteObject(f);
    if (dc_) DeleteDC(dc_);
}

void UiRenderer::begin(int clientWidthPx, int clientHeightPx, double scale) {
    width_ = std::max(clientWidthPx, 1);
    height_ = std::max(clientHeightPx, 1);
    scale_ = scale;
    clips_.clear();
    restoreState();
}

void UiRenderer::restoreState() {
    api_.bindFramebuffer(gl::kFramebuffer, 0);
    glViewport(0, 0, width_, height_);
    glEnable(GL_BLEND);
    api_.blendEquation(gl::kFuncAdd);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    api_.activeTexture(gl::kTexture0);
    api_.bindSampler(0, 0);
    applyClip();
}

void UiRenderer::pushClip(const Rect& r) {
    clips_.push_back(clips_.empty() ? r : clips_.back().intersect(r));
    applyClip();
}

void UiRenderer::popClip() {
    if (!clips_.empty()) clips_.pop_back();
    applyClip();
}

void UiRenderer::applyClip() {
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const Rect& c = clips_.back();
    const int x0 = static_cast<int>(std::lround(c.x * scale_));
    const int y0 = static_cast<int>(std::lround(c.y * scale_));
    const int x1 = static_cast<int>(std::lround(c.right() * scale_));
    const int y1 = static_cast<int>(std::lround(c.bottom() * scale_));
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, height_ - y1, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

// Quad corners in client pixels, converted to NDC; local coordinates at the same corners.
void UiRenderer::shape(int kind, double x0, double y0, double x1, double y1, double lx0, double ly0, double lx1,
                       double ly1, Color c, float p0x, float p0y, float p0z, float p0w, float p1x) {
    if (c.a <= 0.0f) return;
    shape_->use();
    api_.uniform4f(shapeU_.rectNdc, static_cast<float>(x0 / width_ * 2 - 1), static_cast<float>(1 - y0 / height_ * 2),
                   static_cast<float>(x1 / width_ * 2 - 1), static_cast<float>(1 - y1 / height_ * 2));
    api_.uniform4f(shapeU_.localRect, static_cast<float>(lx0), static_cast<float>(ly0), static_cast<float>(lx1),
                   static_cast<float>(ly1));
    api_.uniform1i(shapeU_.kind, kind);
    api_.uniform4f(shapeU_.color, c.r * c.a, c.g * c.a, c.b * c.a, c.a);
    api_.uniform4f(shapeU_.p0, p0x, p0y, p0z, p0w);
    api_.uniform4f(shapeU_.p1, p1x, 0.0f, 0.0f, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void UiRenderer::fill(const Rect& r, Color c, double radius) {
    if (r.empty()) return;
    const double s = scale_;
    const double cx = (r.x + r.w / 2) * s, cy = (r.y + r.h / 2) * s;
    const double hw = r.w * s / 2, hh = r.h * s / 2;
    const double rr = std::min({radius * s, hw, hh});
    shape(0, cx - hw - 1, cy - hh - 1, cx + hw + 1, cy + hh + 1, -hw - 1, -hh - 1, hw + 1, hh + 1, c,
          static_cast<float>(hw), static_cast<float>(hh), static_cast<float>(rr), 0.0f, 0.0f);
}

void UiRenderer::stroke(const Rect& r, Color c, double width, double radius) {
    if (r.empty()) return;
    const double s = scale_;
    const double cx = (r.x + r.w / 2) * s, cy = (r.y + r.h / 2) * s;
    const double hw = r.w * s / 2, hh = r.h * s / 2;
    const double rr = std::min({radius * s, hw, hh});
    shape(0, cx - hw - 1, cy - hh - 1, cx + hw + 1, cy + hh + 1, -hw - 1, -hh - 1, hw + 1, hh + 1, c,
          static_cast<float>(hw), static_cast<float>(hh), static_cast<float>(rr),
          static_cast<float>(std::max(1.0, width * s)), 0.0f);
}

void UiRenderer::line(double x0, double y0, double x1, double y1, double width, Color c) {
    const double s = scale_;
    const double half = std::max(0.5, width * s / 2);
    const double ax = x0 * s, ay = y0 * s, bx = x1 * s, by = y1 * s;
    const double qx0 = std::min(ax, bx) - half - 1, qy0 = std::min(ay, by) - half - 1;
    const double qx1 = std::max(ax, bx) + half + 1, qy1 = std::max(ay, by) + half + 1;
    shape(1, qx0, qy0, qx1, qy1, qx0, qy0, qx1, qy1, c, static_cast<float>(ax), static_cast<float>(ay),
          static_cast<float>(bx), static_cast<float>(by), static_cast<float>(half));
}

void UiRenderer::disc(double cx, double cy, double radius, Color c) {
    const double s = scale_;
    const double r = radius * s;
    shape(2, cx * s - r - 1, cy * s - r - 1, cx * s + r + 1, cy * s + r + 1, -r - 1, -r - 1, r + 1, r + 1, c,
          static_cast<float>(r), 0.0f, 0.0f, 0.0f, 0.0f);
}

void UiRenderer::ring(double cx, double cy, double radius, double width, Color c) {
    const double s = scale_;
    const double r = radius * s;
    shape(2, cx * s - r - 1, cy * s - r - 1, cx * s + r + 1, cy * s + r + 1, -r - 1, -r - 1, r + 1, r + 1, c,
          static_cast<float>(r), static_cast<float>(std::max(1.0, width * s)), 0.0f, 0.0f, 0.0f);
}

void UiRenderer::hueRing(double cx, double cy, double inner, double outer) {
    const double s = scale_;
    const double r = outer * s;
    hue_->use();
    api_.uniform4f(hueU_.rectNdc, static_cast<float>((cx * s - r - 1) / width_ * 2 - 1),
                   static_cast<float>(1 - (cy * s - r - 1) / height_ * 2),
                   static_cast<float>((cx * s + r + 1) / width_ * 2 - 1),
                   static_cast<float>(1 - (cy * s + r + 1) / height_ * 2));
    api_.uniform4f(hueU_.localRect, static_cast<float>(-r - 1), static_cast<float>(-r - 1), static_cast<float>(r + 1),
                   static_cast<float>(r + 1));
    api_.uniform2f(hueU_.radii, static_cast<float>(inner * s), static_cast<float>(r));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void UiRenderer::svSquare(const Rect& r, double hue) {
    const double s = scale_;
    sv_->use();
    api_.uniform4f(svU_.rectNdc, static_cast<float>(r.x * s / width_ * 2 - 1), static_cast<float>(1 - r.y * s / height_ * 2),
                   static_cast<float>(r.right() * s / width_ * 2 - 1),
                   static_cast<float>(1 - r.bottom() * s / height_ * 2));
    api_.uniform4f(svU_.localRect, 0.0f, 0.0f, 1.0f, 1.0f);
    api_.uniform1f(svU_.hue, static_cast<float>(hue));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void UiRenderer::checker(const Rect& r, double cell) {
    const double s = scale_;
    checker_->use();
    api_.uniform4f(checkerU_.rectNdc, static_cast<float>(r.x * s / width_ * 2 - 1),
                   static_cast<float>(1 - r.y * s / height_ * 2), static_cast<float>(r.right() * s / width_ * 2 - 1),
                   static_cast<float>(1 - r.bottom() * s / height_ * 2));
    api_.uniform4f(checkerU_.localRect, 0.0f, 0.0f, static_cast<float>(r.w * s), static_cast<float>(r.h * s));
    api_.uniform1f(checkerU_.cell, static_cast<float>(cell * s));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void UiRenderer::image(const UiImage& picture, const Rect& r) {
    if (picture.width() <= 0) return;
    const double s = scale_;
    image_->use();
    api_.activeTexture(gl::kTexture0);
    glBindTexture(GL_TEXTURE_2D, picture.texture());
    api_.uniform4f(imageU_.rectNdc, static_cast<float>(r.x * s / width_ * 2 - 1), static_cast<float>(1 - r.y * s / height_ * 2),
                   static_cast<float>(r.right() * s / width_ * 2 - 1),
                   static_cast<float>(1 - r.bottom() * s / height_ * 2));
    api_.uniform4f(imageU_.localRect, 0.0f, 0.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// ── Text ─────────────────────────────────────────────────────────────────────

HFONT UiRenderer::font(int px, bool bold) {
    const int key = px * 2 + (bold ? 1 : 0);
    if (const auto it = fonts_.find(key); it != fonts_.end()) return it->second;
    HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Segoe UI");
    fonts_[key] = f;
    return f;
}

double UiRenderer::textWidth(std::wstring_view text, double size, bool bold) {
    if (text.empty()) return 0.0;
    const int px = std::max(6, static_cast<int>(std::lround(size * scale_)));
    SelectObject(dc_, font(px, bold));
    SIZE extent{};
    GetTextExtentPoint32W(dc_, text.data(), static_cast<int>(text.size()), &extent);
    return extent.cx / scale_;
}

double UiRenderer::lineHeight(double size, bool bold) {
    const int px = std::max(6, static_cast<int>(std::lround(size * scale_)));
    SelectObject(dc_, font(px, bold));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc_, &metrics);
    return metrics.tmHeight / scale_;
}

const UiRenderer::TextEntry& UiRenderer::rasterise(std::wstring_view text, int px, bool bold) {
    std::wstring key = std::format(L"{}|{}|", px, bold ? 1 : 0);
    key.append(text);
    if (auto it = textCache_.find(key); it != textCache_.end()) {
        it->second.lastUse = ++useCounter_;
        return it->second;
    }
    if (textCache_.size() >= kMaxCachedTexts) {  // drop the least recently used half
        std::vector<std::pair<std::uint64_t, std::wstring>> ages;
        ages.reserve(textCache_.size());
        for (const auto& [k, e] : textCache_) ages.emplace_back(e.lastUse, k);
        std::sort(ages.begin(), ages.end());
        for (std::size_t i = 0; i < ages.size() / 2; ++i) {
            glDeleteTextures(1, &textCache_[ages[i].second].texture);
            textCache_.erase(ages[i].second);
        }
    }

    SelectObject(dc_, font(px, bold));
    SIZE extent{};
    GetTextExtentPoint32W(dc_, text.data(), static_cast<int>(text.size()), &extent);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc_, &metrics);
    TextEntry entry;
    entry.width = std::max(1, static_cast<int>(extent.cx) + 2);
    entry.height = std::max(1, static_cast<int>(metrics.tmHeight));

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = entry.width;
    info.bmiHeader.biHeight = -entry.height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(entry.width) * static_cast<std::size_t>(entry.height));
    if (bitmap && bits) {
        HGDIOBJ previous = SelectObject(dc_, bitmap);
        SetBkColor(dc_, RGB(0, 0, 0));
        SetTextColor(dc_, RGB(255, 255, 255));
        SetBkMode(dc_, OPAQUE);
        RECT box{0, 0, entry.width, entry.height};
        ExtTextOutW(dc_, 1, 0, ETO_OPAQUE, &box, text.data(), static_cast<UINT>(text.size()), nullptr);
        GdiFlush();
        const auto* pixels = static_cast<const std::uint8_t*>(bits);
        for (std::size_t i = 0; i < coverage.size(); ++i) coverage[i] = pixels[i * 4 + 1];  // greyscale AA: G = coverage
        SelectObject(dc_, previous);
        DeleteObject(bitmap);
    }

    glGenTextures(1, &entry.texture);
    api_.activeTexture(gl::kTexture0);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl::kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl::kClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::kR8), entry.width, entry.height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 coverage.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    entry.lastUse = ++useCounter_;
    return textCache_.emplace(std::move(key), entry).first->second;
}

void UiRenderer::text(std::wstring_view text, double x, double top, double size, Color c, bool bold) {
    if (text.empty() || c.a <= 0.0f) return;
    const int px = std::max(6, static_cast<int>(std::lround(size * scale_)));
    const TextEntry& entry = rasterise(text, px, bold);
    // Pixel-aligned: the coverage texture maps 1:1 onto the screen.
    const double x0 = std::round(x * scale_) - 1;
    const double y0 = std::round(top * scale_);
    const double x1 = x0 + entry.width;
    const double y1 = y0 + entry.height;
    text_->use();
    api_.activeTexture(gl::kTexture0);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    api_.uniform4f(textU_.rectNdc, static_cast<float>(x0 / width_ * 2 - 1), static_cast<float>(1 - y0 / height_ * 2),
                   static_cast<float>(x1 / width_ * 2 - 1), static_cast<float>(1 - y1 / height_ * 2));
    api_.uniform4f(textU_.localRect, 0.0f, 0.0f, 1.0f, 1.0f);
    api_.uniform4f(textU_.color, c.r * c.a, c.g * c.a, c.b * c.a, c.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

std::wstring UiRenderer::elide(std::wstring_view text, double maxWidth, double size, bool bold) {
    if (textWidth(text, size, bold) <= maxWidth) return std::wstring(text);
    std::wstring s(text);
    while (!s.empty()) {
        s.pop_back();
        if (textWidth(s + L"…", size, bold) <= maxWidth) return s + L"…";
    }
    return L"…";
}

void UiRenderer::textIn(std::wstring_view text, const Rect& box, double size, Color c, Align align, bool bold) {
    const std::wstring shown = elide(text, box.w, size, bold);
    const double w = textWidth(shown, size, bold);
    const double h = lineHeight(size, bold);
    double x = box.x;
    if (align == Align::Center) x = box.x + (box.w - w) / 2;
    if (align == Align::Right) x = box.right() - w;
    this->text(shown, x, box.y + (box.h - h) / 2, size, c, bold);
}

}  // namespace chroma::ui

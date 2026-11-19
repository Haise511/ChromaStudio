#pragma once
// ChromaStudio Engine · ui · immediate drawing of the chrome on the GPU.
//
// Everything is specified in client DIP and converted with the window scale, so the chrome is crisp
// at any DPI. Shapes are signed-distance primitives with a one-pixel antialiased edge (rounded
// boxes, strokes, capsules, discs, rings); the colour wheel and the S/V square have their own
// programs (spec §11.1 HSV popup). Text is rasterised by GDI in Segoe UI with greyscale antialiasing
// into cached coverage textures and drawn pixel-aligned. Blending is premultiplied.

#include "render_gl/GlContext.hpp"
#include "render_gl/GlProgram.hpp"
#include "ui/Layout.hpp"
#include "ui/Theme.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chroma::ui {

enum class Align : std::uint8_t { Left, Center, Right };

/// An RGBA8 picture with straight alpha that the chrome can show (layer thumbnails).
class UiImage {
public:
    UiImage(const gl::Context& context);
    ~UiImage();
    UiImage(const UiImage&) = delete;
    UiImage& operator=(const UiImage&) = delete;
    void update(int width, int height, const std::uint8_t* rgba);
    [[nodiscard]] GLuint texture() const noexcept { return texture_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    std::weak_ptr<const void> lifetime_;
    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;
};

class UiRenderer {
public:
    explicit UiRenderer(const gl::Context& context);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    /// Starts a frame on the default framebuffer.
    void begin(int clientWidthPx, int clientHeightPx, double scale);
    /// Re-applies the chrome's GL state after other drawing (the document) in the same frame.
    void restoreState();
    [[nodiscard]] double scale() const noexcept { return scale_; }
    [[nodiscard]] int clientWidth() const noexcept { return width_; }   // physical px of this frame
    [[nodiscard]] int clientHeight() const noexcept { return height_; }

    void pushClip(const Rect& r);
    void popClip();

    void fill(const Rect& r, Color c, double radius = 0.0);
    void stroke(const Rect& r, Color c, double width, double radius = 0.0);
    void line(double x0, double y0, double x1, double y1, double width, Color c);
    void disc(double cx, double cy, double radius, Color c);
    void ring(double cx, double cy, double radius, double width, Color c);
    void hueRing(double cx, double cy, double inner, double outer);
    void svSquare(const Rect& r, double hue);
    void checker(const Rect& r, double cell);
    void image(const UiImage& picture, const Rect& r);

    // Text: size is the em height in DIP.
    [[nodiscard]] double textWidth(std::wstring_view text, double size, bool bold = false);
    [[nodiscard]] double lineHeight(double size, bool bold = false);
    void text(std::wstring_view text, double x, double top, double size, Color c, bool bold = false);
    /// Vertically centred in `box`, aligned horizontally, elided with "…" when too long.
    void textIn(std::wstring_view text, const Rect& box, double size, Color c, Align align, bool bold = false);
    [[nodiscard]] std::wstring elide(std::wstring_view text, double maxWidth, double size, bool bold = false);

private:
    struct TextEntry {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        std::uint64_t lastUse = 0;
    };

    void shape(int kind, double x0, double y0, double x1, double y1, double lx0, double ly0, double lx1, double ly1,
               Color c, float p0x, float p0y, float p0z, float p0w, float p1x);
    void applyClip();
    HFONT font(int px, bool bold);
    const TextEntry& rasterise(std::wstring_view text, int px, bool bold);

    const gl::Api& api_;
    std::weak_ptr<const void> lifetime_;
    std::unique_ptr<gl::Program> shape_;
    std::unique_ptr<gl::Program> text_;
    std::unique_ptr<gl::Program> hue_;
    std::unique_ptr<gl::Program> sv_;
    std::unique_ptr<gl::Program> checker_;
    std::unique_ptr<gl::Program> image_;
    struct {
        GLint rectNdc, localRect, kind, color, p0, p1;
    } shapeU_{};
    struct {
        GLint rectNdc, localRect, color;
    } textU_{};
    struct {
        GLint rectNdc, localRect, radii;
    } hueU_{};
    struct {
        GLint rectNdc, localRect, hue;
    } svU_{};
    struct {
        GLint rectNdc, localRect, cell;
    } checkerU_{};
    struct {
        GLint rectNdc, localRect;
    } imageU_{};

    int width_ = 1;
    int height_ = 1;
    double scale_ = 1.0;
    std::vector<Rect> clips_;

    HDC dc_ = nullptr;
    std::unordered_map<int, HFONT> fonts_;  // key: px * 2 + bold
    std::unordered_map<std::wstring, TextEntry> textCache_;
    std::uint64_t useCounter_ = 0;
};

}  // namespace chroma::ui

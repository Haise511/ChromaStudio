#pragma once
// ChromaStudio Engine · render_gl · WGL context and local function loader (spec §2, §12 render_gl).
//
// One desktop OpenGL 3.3 core context per window. A temporary legacy context fetches
// wglCreateContextAttribsARB; the real context is double buffered, robust when the driver offers it,
// and must render to RGBA16F and R16F (checked by read-back). GL 1.1 entry points come from
// opengl32 (<GL/gl.h>); everything newer is resolved at run time into Api.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <GL/gl.h>

#include <memory>
#include <string>
#include <string_view>

namespace chroma::gl {

using GLchar = char;
using DebugProc = void(APIENTRY*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                  const GLchar* message, const void* user);

// GL 1.2+ enumerants (Khronos registry, glcorearb.h).
inline constexpr GLenum kHalfFloat = 0x140B;
inline constexpr GLenum kFuncAdd = 0x8006;
inline constexpr GLenum kMax = 0x8008;
inline constexpr GLenum kClampToEdge = 0x812F;
inline constexpr GLenum kMajorVersion = 0x821B;
inline constexpr GLenum kMinorVersion = 0x821C;
inline constexpr GLenum kNumExtensions = 0x821D;
inline constexpr GLenum kR8 = 0x8229;
inline constexpr GLenum kR16f = 0x822D;
inline constexpr GLenum kDebugOutputSynchronous = 0x8242;
inline constexpr GLenum kDebugSeverityNotification = 0x826B;
inline constexpr GLenum kTexture0 = 0x84C0;
inline constexpr GLenum kMaxRenderbufferSize = 0x84E8;
inline constexpr GLenum kRgba32f = 0x8814;
inline constexpr GLenum kRgba16f = 0x881A;
inline constexpr GLenum kFragmentShader = 0x8B30;
inline constexpr GLenum kVertexShader = 0x8B31;
inline constexpr GLenum kCompileStatus = 0x8B81;
inline constexpr GLenum kLinkStatus = 0x8B82;
inline constexpr GLenum kInfoLogLength = 0x8B84;
inline constexpr GLenum kShadingLanguageVersion = 0x8B8C;
inline constexpr GLenum kReadFramebuffer = 0x8CA8;
inline constexpr GLenum kDrawFramebuffer = 0x8CA9;
inline constexpr GLenum kFramebufferComplete = 0x8CD5;
inline constexpr GLenum kColorAttachment0 = 0x8CE0;
inline constexpr GLenum kFramebuffer = 0x8D40;
inline constexpr GLenum kDebugOutput = 0x92E0;

struct Api {
    // Vertex arrays, framebuffers and blits.
    void(APIENTRY* genVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* bindVertexArray)(GLuint) = nullptr;
    void(APIENTRY* deleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* genFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* deleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum(APIENTRY* checkFramebufferStatus)(GLenum) = nullptr;
    void(APIENTRY* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                    GLenum) = nullptr;
    const GLubyte*(APIENTRY* getStringi)(GLenum, GLuint) = nullptr;
    // Textures, samplers, blending.
    void(APIENTRY* activeTexture)(GLenum) = nullptr;
    void(APIENTRY* blendEquation)(GLenum) = nullptr;
    void(APIENTRY* blendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void(APIENTRY* genSamplers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteSamplers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindSampler)(GLuint, GLuint) = nullptr;
    void(APIENTRY* samplerParameteri)(GLuint, GLenum, GLint) = nullptr;
    // Shaders and programs.
    GLuint(APIENTRY* createShader)(GLenum) = nullptr;
    void(APIENTRY* shaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(APIENTRY* compileShader)(GLuint) = nullptr;
    void(APIENTRY* getShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* getShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* deleteShader)(GLuint) = nullptr;
    GLuint(APIENTRY* createProgram)() = nullptr;
    void(APIENTRY* attachShader)(GLuint, GLuint) = nullptr;
    void(APIENTRY* linkProgram)(GLuint) = nullptr;
    void(APIENTRY* getProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* getProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* deleteProgram)(GLuint) = nullptr;
    void(APIENTRY* useProgram)(GLuint) = nullptr;
    GLint(APIENTRY* getUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(APIENTRY* uniform1i)(GLint, GLint) = nullptr;
    void(APIENTRY* uniform1ui)(GLint, GLuint) = nullptr;
    void(APIENTRY* uniform1f)(GLint, GLfloat) = nullptr;
    void(APIENTRY* uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void(APIENTRY* uniform2i)(GLint, GLint, GLint) = nullptr;
    void(APIENTRY* uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
    void(APIENTRY* uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    // Optional.
    GLenum(APIENTRY* getGraphicsResetStatus)() = nullptr;                    // robustness
    void(APIENTRY* debugMessageCallback)(DebugProc, const void*) = nullptr;  // KHR_debug
};

class Context {
public:
    /// Creates and makes current an OpenGL 3.3 core context on `window` (CS_OWNDC). Returns nullptr
    /// with a reason when the GPU path is unavailable.
    static std::unique_ptr<Context> create(HWND window, std::string& failure);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] const Api& api() const noexcept { return api_; }
    [[nodiscard]] std::string_view label() const noexcept { return label_; }
    [[nodiscard]] GLint maxTextureSize() const noexcept { return maxTextureSize_; }
    [[nodiscard]] bool deviceLost() const;

    /// SwapBuffers, then wait for the frame: one frame in flight keeps input latency to one vblank.
    bool present();

    bool makeCurrent() const { return wglMakeCurrent(dc_, rc_) != FALSE; }

    /// Expires when the context is destroyed; GPU objects check it before deleting themselves.
    [[nodiscard]] std::weak_ptr<const void> lifetime() const { return alive_; }

    struct WglEntryPoints;

private:
    Context(HWND window, HDC dc) : window_(window), dc_(dc) {}
    bool initialise(const WglEntryPoints& wgl, std::string& failure);
    [[nodiscard]] HGLRC createContext(const WglEntryPoints& wgl, bool robust) const;
    bool loadApi(std::string& failure);
    bool probeRenderTarget(GLenum internalFormat, GLenum format, std::string& failure);

    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    Api api_{};
    bool robust_ = false;
    GLuint vao_ = 0;
    GLint maxTextureSize_ = 0;
    std::string label_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace chroma::gl

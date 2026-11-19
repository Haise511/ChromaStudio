#include "render_gl/GlContext.hpp"

#include "core/Log.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

namespace chroma::gl {
namespace {

// WGL enumerants (wglext.h).
constexpr int kWglDrawToWindow = 0x2001;
constexpr int kWglAcceleration = 0x2003;
constexpr int kWglSupportOpenGl = 0x2010;
constexpr int kWglDoubleBuffer = 0x2011;
constexpr int kWglPixelType = 0x2013;
constexpr int kWglRedBits = 0x2015;
constexpr int kWglGreenBits = 0x2017;
constexpr int kWglBlueBits = 0x2019;
constexpr int kWglAlphaBits = 0x201B;
constexpr int kWglDepthBits = 0x2022;
constexpr int kWglStencilBits = 0x2023;
constexpr int kWglFullAcceleration = 0x2027;
constexpr int kWglTypeRgba = 0x202B;
constexpr int kWglSampleBuffers = 0x2041;
constexpr int kWglContextMajorVersion = 0x2091;
constexpr int kWglContextMinorVersion = 0x2092;
constexpr int kWglContextFlags = 0x2094;
constexpr int kWglContextDebugBit = 0x0001;
constexpr int kWglContextRobustAccessBit = 0x0004;
constexpr int kWglContextLoseOnReset = 0x8252;
constexpr int kWglContextResetStrategy = 0x8256;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfileBit = 0x0001;

using ChoosePixelFormatArb = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using CreateContextAttribsArb = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using GetExtensionsStringArb = const char*(WINAPI*)(HDC);
using SwapIntervalExt = BOOL(WINAPI*)(int);

// wglGetProcAddress returns 0, 1, 2, 3 or -1 for names it does not resolve; GL 1.1 entry points live
// in opengl32.dll itself.
template <class Fn>
bool loadProc(Fn& out, const char* name) {
    PROC proc = wglGetProcAddress(name);
    const auto bits = reinterpret_cast<std::intptr_t>(proc);
    if (bits >= -1 && bits <= 3) proc = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    if (!proc) {
        out = nullptr;
        return false;
    }
    static_assert(sizeof(Fn) == sizeof(proc));
    std::memcpy(&out, &proc, sizeof(out));
    return true;
}

bool hasExtension(std::string_view list, std::string_view name) {
    for (std::size_t pos = list.find(name); pos != std::string_view::npos; pos = list.find(name, pos + 1)) {
        const bool startOk = pos == 0 || list[pos - 1] == ' ';
        const std::size_t end = pos + name.size();
        if (startOk && (end == list.size() || list[end] == ' ')) return true;
    }
    return false;
}

void APIENTRY onDebugMessage(GLenum, GLenum, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == kDebugSeverityNotification) return;
    logf("GL debug [{}]: {}", id, message ? message : "");
}

}  // namespace

struct Context::WglEntryPoints {
    ChoosePixelFormatArb choosePixelFormat = nullptr;
    CreateContextAttribsArb createContextAttribs = nullptr;
    std::string extensions;
};

namespace {

// wglCreateContextAttribsARB is reachable only through a current legacy context, and a window
// accepts SetPixelFormat once, so the bootstrap runs on its own hidden window (spec §2).
bool loadWglEntryPoints(Context::WglEntryPoints& out, std::string& failure) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"ChromaStudio.WglBootstrap";
    RegisterClassW(&wc);  // ERROR_CLASS_ALREADY_EXISTS on a later attempt is fine

    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) {
        failure = "cannot create the WGL bootstrap window";
        return false;
    }
    HDC dc = GetDC(window);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    HGLRC legacy = nullptr;
    const int format = ChoosePixelFormat(dc, &pfd);
    if (format != 0 && SetPixelFormat(dc, format, &pfd)) legacy = wglCreateContext(dc);

    bool ok = false;
    if (legacy && wglMakeCurrent(dc, legacy)) {
        loadProc(out.choosePixelFormat, "wglChoosePixelFormatARB");
        loadProc(out.createContextAttribs, "wglCreateContextAttribsARB");
        GetExtensionsStringArb getExtensions = nullptr;
        if (loadProc(getExtensions, "wglGetExtensionsStringARB")) {
            if (const char* list = getExtensions(dc)) out.extensions = list;
        }
        ok = out.choosePixelFormat && out.createContextAttribs;
        if (!ok) failure = "the driver lacks WGL_ARB_pixel_format / WGL_ARB_create_context";
    } else {
        failure = "no hardware OpenGL driver (legacy bootstrap context failed)";
    }

    wglMakeCurrent(nullptr, nullptr);
    if (legacy) wglDeleteContext(legacy);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return ok;
}

}  // namespace

std::unique_ptr<Context> Context::create(HWND window, std::string& failure) {
    WglEntryPoints wgl;
    if (!loadWglEntryPoints(wgl, failure)) return nullptr;
    HDC dc = GetDC(window);  // CS_OWNDC: valid for the window's lifetime
    if (!dc) {
        failure = "GetDC failed";
        return nullptr;
    }
    std::unique_ptr<Context> context(new Context(window, dc));
    if (!context->initialise(wgl, failure)) return nullptr;
    return context;
}

Context::~Context() {
    alive_.reset();
    if (rc_) {
        if (wglGetCurrentContext() == rc_ && vao_ && api_.deleteVertexArrays) api_.deleteVertexArrays(1, &vao_);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc_);
    }
    if (dc_) ReleaseDC(window_, dc_);
}

bool Context::deviceLost() const {
    return robust_ && api_.getGraphicsResetStatus && api_.getGraphicsResetStatus() != GL_NO_ERROR;
}

bool Context::present() {
    glDisable(GL_SCISSOR_TEST);
    if (!SwapBuffers(dc_)) return false;
    // One frame in flight: the next frame starts from the newest packets instead of queuing behind
    // the driver's render-ahead.
    glFinish();
    return !deviceLost();
}

bool Context::initialise(const WglEntryPoints& wgl, std::string& failure) {
    // A window accepts one pixel format for its lifetime; a context re-created after device loss
    // reuses the one already set.
    if (GetPixelFormat(dc_) == 0) {
        std::vector<int> attributes = {
            kWglDrawToWindow, 1, kWglSupportOpenGl, 1, kWglDoubleBuffer, 1,
            kWglAcceleration, kWglFullAcceleration, kWglPixelType, kWglTypeRgba,
            kWglRedBits, 8, kWglGreenBits, 8, kWglBlueBits, 8, kWglAlphaBits, 8,
            kWglDepthBits, 0, kWglStencilBits, 0,
        };
        if (hasExtension(wgl.extensions, "WGL_ARB_multisample")) {
            attributes.insert(attributes.end(), {kWglSampleBuffers, 0});
        }
        attributes.push_back(0);
        int format = 0;
        UINT count = 0;
        if (!wgl.choosePixelFormat(dc_, attributes.data(), nullptr, 1, &format, &count) || count == 0) {
            failure = "no accelerated double-buffered RGBA8 pixel format";
            return false;
        }
        PIXELFORMATDESCRIPTOR pfd{};
        DescribePixelFormat(dc_, format, sizeof(pfd), &pfd);
        if (!SetPixelFormat(dc_, format, &pfd)) {
            failure = std::format("SetPixelFormat failed (error {})", GetLastError());
            return false;
        }
    }

    robust_ = hasExtension(wgl.extensions, "WGL_ARB_create_context_robustness");
    rc_ = createContext(wgl, robust_);
    if (!rc_ && robust_) {
        robust_ = false;
        rc_ = createContext(wgl, false);
    }
    if (!rc_) {
        failure = "wglCreateContextAttribsARB could not create an OpenGL 3.3 core context";
        return false;
    }
    if (!wglMakeCurrent(dc_, rc_)) {
        failure = std::format("wglMakeCurrent failed (error {})", GetLastError());
        return false;
    }

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(kMajorVersion, &major);
    glGetIntegerv(kMinorVersion, &minor);
    if (major * 10 + minor < 33) {
        failure = std::format("OpenGL {}.{} context, 3.3 required", major, minor);
        return false;
    }
    if (!loadApi(failure)) return false;

    SwapIntervalExt swapInterval = nullptr;
    if (hasExtension(wgl.extensions, "WGL_EXT_swap_control") && loadProc(swapInterval, "wglSwapIntervalEXT")) {
        swapInterval(1);
    }
#ifndef NDEBUG
    if (api_.debugMessageCallback) {
        glEnable(kDebugOutput);
        glEnable(kDebugOutputSynchronous);
        api_.debugMessageCallback(&onDebugMessage, nullptr);
    }
#endif
    // Core profile: a VAO is part of the state every program draws with (spec §2, §9.1).
    api_.genVertexArrays(1, &vao_);
    api_.bindVertexArray(vao_);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize_);
    GLint maxRenderbuffer = 0;
    glGetIntegerv(kMaxRenderbufferSize, &maxRenderbuffer);
    if (maxTextureSize_ < 1024) {  // below the OpenGL 3.3 minimum: a broken driver
        failure = std::format("GL_MAX_TEXTURE_SIZE {} is below the OpenGL 3.3 minimum", maxTextureSize_);
        return false;
    }
    // Spec §2: document tiles are RGBA16F; stamp weights are R16F.
    if (!probeRenderTarget(kRgba16f, GL_RGBA, failure) || !probeRenderTarget(kR16f, GL_RED, failure)) return false;

    const auto text = [](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return std::string(value ? value : "?");
    };
    label_ = std::format("OpenGL {}.{} core", major, minor);
    logf("GL: {} | {} | {}", text(GL_VENDOR), text(GL_RENDERER), text(GL_VERSION));
    logf("GL: GLSL {} | max texture {} | max renderbuffer {} | robust {} | swap interval {}",
         text(kShadingLanguageVersion), maxTextureSize_, maxRenderbuffer,
         robust_ && api_.getGraphicsResetStatus ? "yes" : "no", swapInterval ? "1" : "driver default");
    logf("GL: RGBA16F and R16F 128x128 render targets verified by read-back");
    return true;
}

HGLRC Context::createContext(const WglEntryPoints& wgl, bool robust) const {
    int flags = 0;
#ifndef NDEBUG
    flags |= kWglContextDebugBit;
#endif
    if (robust) flags |= kWglContextRobustAccessBit;
    std::vector<int> attributes = {
        kWglContextMajorVersion, 3, kWglContextMinorVersion, 3,
        kWglContextProfileMask, kWglContextCoreProfileBit, kWglContextFlags, flags,
    };
    if (robust) attributes.insert(attributes.end(), {kWglContextResetStrategy, kWglContextLoseOnReset});
    attributes.push_back(0);
    return wgl.createContextAttribs(dc_, nullptr, attributes.data());
}

bool Context::loadApi(std::string& failure) {
    std::string missing;
    const auto require = [&missing](auto& fn, const char* name) {
        if (!loadProc(fn, name)) missing += std::format(" {}", name);
    };
    require(api_.genVertexArrays, "glGenVertexArrays");
    require(api_.bindVertexArray, "glBindVertexArray");
    require(api_.deleteVertexArrays, "glDeleteVertexArrays");
    require(api_.genFramebuffers, "glGenFramebuffers");
    require(api_.bindFramebuffer, "glBindFramebuffer");
    require(api_.deleteFramebuffers, "glDeleteFramebuffers");
    require(api_.framebufferTexture2D, "glFramebufferTexture2D");
    require(api_.checkFramebufferStatus, "glCheckFramebufferStatus");
    require(api_.blitFramebuffer, "glBlitFramebuffer");
    require(api_.getStringi, "glGetStringi");
    require(api_.activeTexture, "glActiveTexture");
    require(api_.blendEquation, "glBlendEquation");
    require(api_.blendFuncSeparate, "glBlendFuncSeparate");
    require(api_.genSamplers, "glGenSamplers");
    require(api_.deleteSamplers, "glDeleteSamplers");
    require(api_.bindSampler, "glBindSampler");
    require(api_.samplerParameteri, "glSamplerParameteri");
    require(api_.createShader, "glCreateShader");
    require(api_.shaderSource, "glShaderSource");
    require(api_.compileShader, "glCompileShader");
    require(api_.getShaderiv, "glGetShaderiv");
    require(api_.getShaderInfoLog, "glGetShaderInfoLog");
    require(api_.deleteShader, "glDeleteShader");
    require(api_.createProgram, "glCreateProgram");
    require(api_.attachShader, "glAttachShader");
    require(api_.linkProgram, "glLinkProgram");
    require(api_.getProgramiv, "glGetProgramiv");
    require(api_.getProgramInfoLog, "glGetProgramInfoLog");
    require(api_.deleteProgram, "glDeleteProgram");
    require(api_.useProgram, "glUseProgram");
    require(api_.getUniformLocation, "glGetUniformLocation");
    require(api_.uniform1i, "glUniform1i");
    require(api_.uniform1ui, "glUniform1ui");
    require(api_.uniform1f, "glUniform1f");
    require(api_.uniform2f, "glUniform2f");
    require(api_.uniform2i, "glUniform2i");
    require(api_.uniform3f, "glUniform3f");
    require(api_.uniform4f, "glUniform4f");
    if (!missing.empty()) {
        failure = "missing OpenGL entry points:" + missing;
        return false;
    }
    if (robust_ && !loadProc(api_.getGraphicsResetStatus, "glGetGraphicsResetStatus")) {
        loadProc(api_.getGraphicsResetStatus, "glGetGraphicsResetStatusARB");
    }
    if (!loadProc(api_.debugMessageCallback, "glDebugMessageCallback")) {
        loadProc(api_.debugMessageCallback, "glDebugMessageCallbackARB");
    }
    return true;
}

// Clears a 128×128 target to values binary16 holds exactly and reads one pixel back.
bool Context::probeRenderTarget(GLenum internalFormat, GLenum format, std::string& failure) {
    constexpr int kSize = 128;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), kSize, kSize, 0, format, GL_FLOAT, nullptr);
    api_.genFramebuffers(1, &framebuffer);
    api_.bindFramebuffer(kFramebuffer, framebuffer);
    api_.framebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, texture, 0);

    const char* name = internalFormat == kRgba16f ? "RGBA16F" : "R16F";
    bool ok = false;
    const GLenum status = api_.checkFramebufferStatus(kFramebuffer);
    if (status == kFramebufferComplete) {
        constexpr std::array<float, 4> expected{0.25f, 0.5f, 0.75f, 1.0f};
        std::array<float, 4> pixel{-1.0f, -1.0f, -1.0f, -1.0f};
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(expected[0], expected[1], expected[2], expected[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(kSize / 2, kSize / 2, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
        // A single-channel target reads back as (r, 0, 0, 1).
        ok = format == GL_RED ? pixel[0] == expected[0] : pixel == expected;
        if (!ok) failure = std::format("{} read-back mismatch: ({}, {}, {}, {})", name, pixel[0], pixel[1], pixel[2], pixel[3]);
    } else {
        failure = std::format("{} framebuffer incomplete (0x{:04X})", name, status);
    }
    api_.bindFramebuffer(kFramebuffer, 0);
    api_.deleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    return ok;
}

}  // namespace chroma::gl

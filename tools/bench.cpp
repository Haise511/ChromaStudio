// ChromaStudio Engine · the §12.2 release measurement (not part of CTest).
//
//   chroma_bench [--seconds=30] [--zoom=fit|1]
//
// Scene: a 4096² canvas, 12 raster layers with 25 % of their tiles occupied, a 1440 × 900 hidden
// window, an input journal at 240 Hz and output at 60 Hz, the built-in pen at D = 64 and spacing .1.
// After a 3-s warm-up it measures for the given time and prints p50/p95/p99 of
//   - CPU frame preparation: stroke building, stamping and composing calls until present,
//   - GPU frame time (GL_TIME_ELAPSED around the frame's GL work),
//   - "packet received → frame presented": present() returns after the frame is done (one frame in
//     flight, as in the editor), so this includes the wait for the next 60-Hz frame.
// The first stroke after start is reported apart, as is the peak working set. The editor's chrome
// is not drawn here; its cost is a few hundred SDF quads per frame.

#include "core/BrushProfile.hpp"
#include "core/Half.hpp"
#include "core/LayerTree.hpp"
#include "core/StrokeBuilder.hpp"
#include "render_gl/GlContext.hpp"
#include "render_gl/TileRenderer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace chroma;

constexpr int kDoc = 4096;
constexpr int kLayers = 12;
constexpr int kWindowW = 1440;
constexpr int kWindowH = 900;
constexpr double kInputHz = 240.0;
constexpr double kOutputHz = 60.0;
constexpr GLenum kTimeElapsed = 0x88BF;
constexpr GLenum kQueryResult = 0x8866;

using GenQueries = void(APIENTRY*)(GLsizei, GLuint*);
using BeginQuery = void(APIENTRY*)(GLenum, GLuint);
using EndQuery = void(APIENTRY*)(GLenum);
using GetQueryObjectui64v = void(APIENTRY*)(GLuint, GLenum, std::uint64_t*);

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Stats {
    std::vector<double> values;
    [[nodiscard]] double pct(double p) const {
        if (values.empty()) return 0.0;
        std::vector<double> v = values;
        std::sort(v.begin(), v.end());
        const auto i = static_cast<std::size_t>(std::clamp(std::ceil(p / 100.0 * v.size()) - 1.0, 0.0, v.size() - 1.0));
        return v[i];
    }
    void print(const char* name, double budget) const {
        std::printf("  %-34s p50 %7.2f   p95 %7.2f   p99 %7.2f ms   (%zu samples%s)\n", name, pct(50), pct(95), pct(99),
                    values.size(), budget > 0 ? (pct(95) <= budget ? ", p95 within budget" : ", p95 OVER budget") : "");
    }
};

std::string cpuName() {
    char name[256] = {};
    DWORD size = sizeof name;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, name, &size);
    return name;
}

int physicalCores() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<std::byte> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) {
        return 0;
    }
    int cores = 0;
    for (DWORD at = 0; at < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        ++cores;
        at += info->Size;
    }
    return cores;
}

/// A crash prints the faulting stack with file and line (the bench runs without a debugger).
LONG WINAPI crashReport(EXCEPTION_POINTERS* info) {
    std::printf("crash: exception 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD column = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
        const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &column, &line) != FALSE;
        std::printf("  #%d %s  %s:%lu\n", i, named ? symbol->Name : "?", lined ? line.FileName : "?", lined ? line.LineNumber : 0);
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crashReport);
    SetConsoleOutputCP(CP_UTF8);
    double seconds = 30.0;
    bool fit = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("--seconds=")) seconds = std::max(1.0, std::stod(arg.substr(10)));
        if (arg == "--zoom=1") fit = false;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ChromaStudio.Bench";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, kWindowW, kWindowH, nullptr, nullptr,
                                  wc.hInstance, nullptr);  // never shown
    std::string failure = "no window";
    const Clock::time_point launch = Clock::now();
    std::unique_ptr<gl::Context> context = window ? gl::Context::create(window, failure) : nullptr;
    if (!context) {
        std::printf("no OpenGL 3.3 context: %s\n", failure.c_str());
        return 1;
    }
    const auto genQueries = reinterpret_cast<GenQueries>(wglGetProcAddress("glGenQueries"));
    const auto beginQuery = reinterpret_cast<BeginQuery>(wglGetProcAddress("glBeginQuery"));
    const auto endQuery = reinterpret_cast<EndQuery>(wglGetProcAddress("glEndQuery"));
    const auto queryResult = reinterpret_cast<GetQueryObjectui64v>(wglGetProcAddress("glGetQueryObjectui64v"));
    GLuint query = 0;
    if (genQueries) genQueries(1, &query);
    const Clock::time_point contextReady = Clock::now();

    // ── Scene ──
    core::LayerTree tree = core::LayerTree::single();
    std::vector<std::uint64_t> layers = {tree.rasters().front()};
    while (layers.size() < kLayers) layers.push_back(tree.addRaster("Слой " + std::to_string(layers.size() + 1)));
    std::map<std::tuple<std::uint64_t, int, int>, render::TileRef> tiles;
    std::mt19937 random(1847);
    const int grid = kDoc / render::kTileSize;
    for (const std::uint64_t layer : layers) {
        std::vector<int> cells(static_cast<std::size_t>(grid * grid));
        for (int i = 0; i < grid * grid; ++i) cells[static_cast<std::size_t>(i)] = i;
        std::shuffle(cells.begin(), cells.end(), random);
        const float hue = std::uniform_real_distribution<float>(0.0f, 1.0f)(random);
        for (int i = 0; i < grid * grid / 4; ++i) {
            auto data = std::make_shared<core::TileData>();
            const float a = 0.35f + 0.5f * std::uniform_real_distribution<float>(0.0f, 1.0f)(random);
            for (std::size_t p = 0; p < static_cast<std::size_t>(render::kTileSize) * render::kTileSize; ++p) {
                const float shade = 0.5f + 0.5f * std::sin(static_cast<float>(p % 128) * 0.05f + hue * 6.0f);
                data->halfs[p * 4] = core::floatToHalf(a * shade * hue);
                data->halfs[p * 4 + 1] = core::floatToHalf(a * shade * (1.0f - hue));
                data->halfs[p * 4 + 2] = core::floatToHalf(a * 0.3f);
                data->halfs[p * 4 + 3] = core::floatToHalf(a);
            }
            const int cell = cells[static_cast<std::size_t>(i)];
            tiles[{layer, cell % grid, cell / grid}] = render::TileRenderer::makeVersion(data);
        }
    }
    tree.setActive(layers.back());
    render::LayerStack stack;
    stack.tree = &tree;
    stack.tiles = [&](std::uint64_t layer, int tx, int ty) -> render::TileRef {
        const auto it = tiles.find({layer, tx, ty});
        return it == tiles.end() ? nullptr : it->second;
    };
    const render::TileLookup committed = [&](int tx, int ty) { return stack.tiles(layers.back(), tx, ty); };

    const Clock::time_point sceneReady = Clock::now();
    render::TileRenderer renderer(*context, kDoc, kDoc);
    const Clock::time_point programsReady = Clock::now();
    render::DocumentFrame frame;
    frame.clientWidth = kWindowW;
    frame.clientHeight = kWindowH;
    frame.viewportX1 = kWindowW;
    frame.viewportY1 = kWindowH;
    frame.pixelsPerDocPx = fit ? std::min(kWindowW, kWindowH) / static_cast<double>(kDoc) : 1.0;
    frame.originX = fit ? (kWindowW - kDoc * frame.pixelsPerDocPx) / 2 : -(kDoc / 2.0 - kWindowW / 2.0);
    frame.originY = fit ? (kWindowH - kDoc * frame.pixelsPerDocPx) / 2 : -(kDoc / 2.0 - kWindowH / 2.0);
    const double viewX0 = -frame.originX / frame.pixelsPerDocPx, viewY0 = -frame.originY / frame.pixelsPerDocPx;
    const double viewW = kWindowW / frame.pixelsPerDocPx, viewH = kWindowH / frame.pixelsPerDocPx;
    const int visibleTiles = static_cast<int>((std::ceil(std::min<double>(kDoc, viewX0 + viewW) / 128) - std::floor(std::max(0.0, viewX0) / 128)) *
                                              (std::ceil(std::min<double>(kDoc, viewY0 + viewH) / 128) - std::floor(std::max(0.0, viewY0) / 128)));

    // The built-in pen at D = 64, spacing .1 (§12.2).
    core::BrushProfile pen = core::builtInBrushes().front();
    pen.tip.diameter = 64;
    pen.ink.spacing = 0.1;
    const render::BrushSetup setup = render::brushSetupFor(pen, {0.05f, 0.05f, 0.08f, 1.0f}, false);

    // Input journal: 2-s strokes along a sine across the visible part, 240 packets per second.
    const auto pathAt = [&](double t) {
        const double u = std::fmod(t, 2.0) / 2.0;
        const double x = viewX0 + viewW * (0.1 + 0.8 * u);
        const double y = viewY0 + viewH * (0.5 + 0.3 * std::sin(2 * std::numbers::pi * (u * 2 + std::floor(t / 2.0) * 0.37)));
        return core::V{std::clamp(x, 1.0, kDoc - 1.0), std::clamp(y, 1.0, kDoc - 1.0)};
    };

    // The document is on screen before the first stroke, as in the editor after opening a file.
    glViewport(0, 0, kWindowW, kWindowH);
    renderer.drawDocument(frame, stack);
    context->present();
    const Clock::time_point firstFrame = Clock::now();

    Stats cpu, gpu, latency;
    double firstStroke = -1.0;
    const double warmup = 3.0, total = warmup + seconds;
    const Clock::time_point start = Clock::now();
    std::unique_ptr<core::StrokeBuilder> builder;
    double strokeStart = 0.0;
    std::size_t nextPacket = 0;
    std::vector<double> pending;  // arrival times (s) of packets not yet presented
    std::vector<core::Dab> dabs, tail;
    for (std::size_t frameIndex = 0;; ++frameIndex) {
        // 60-Hz output: wait for this frame's slot.
        const Clock::time_point slot = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frameIndex / kOutputHz));
        std::this_thread::sleep_until(slot);
        const Clock::time_point frameStart = Clock::now();
        const double now = std::chrono::duration<double>(frameStart - start).count();
        if (now > total) break;
        const bool measuring = now >= warmup;

        if (query && beginQuery) beginQuery(kTimeElapsed, query);
        // Packets that arrived by now.
        for (; static_cast<double>(nextPacket) / kInputHz <= now; ++nextPacket) {
            const double t = static_cast<double>(nextPacket) / kInputHz;
            if (!builder || std::floor(t / 2.0) != std::floor(strokeStart / 2.0)) {
                if (builder) {
                    (void)builder->finish({t * 1000.0, pathAt(t - 1e-6), 0.0});
                    renderer.setTail({});
                    for (const render::TileChange& c : renderer.commitStroke()) {
                        if (c.after) tiles[{c.key.layer, c.key.tx, c.key.ty}] = c.after;
                    }
                }
                builder = std::make_unique<core::StrokeBuilder>(pen, pen.tip.diameter);
                strokeStart = t;
                render::BrushSetup s = setup;
                s.layer = layers.back();
                renderer.beginStroke(s, committed);
            }
            const double pressure = 0.35 + 0.6 * std::sin(std::numbers::pi * std::fmod(t, 2.0) / 2.0);
            dabs.clear();
            builder->add({t * 1000.0, pathAt(t), pressure}, dabs);
            renderer.applyDabs(dabs);
            pending.push_back(t);
        }
        if (builder) {
            tail.clear();
            builder->tail(tail);
            renderer.setTail(tail);
        }
        glViewport(0, 0, kWindowW, kWindowH);
        renderer.drawDocument(frame, stack);
        if (query && endQuery) endQuery(kTimeElapsed);
        const Clock::time_point prepared = Clock::now();
        context->present();
        const Clock::time_point presented = Clock::now();
        const double shown = std::chrono::duration<double>(presented - start).count();
        if (firstStroke < 0 && !pending.empty()) firstStroke = (shown - pending.front()) * 1000.0;
        if (measuring) {
            cpu.values.push_back(ms(prepared - frameStart));
            for (const double t : pending) latency.values.push_back((shown - t) * 1000.0);
            if (query && queryResult) {
                std::uint64_t ns = 0;
                queryResult(query, kQueryResult, &ns);
                gpu.values.push_back(static_cast<double>(ns) / 1e6);
            }
        }
        pending.clear();
    }
    if (builder) {
        renderer.setTail({});
        (void)renderer.commitStroke();
    }

    PROCESS_MEMORY_COUNTERS memory{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory);
    MEMORYSTATUSEX ram{};
    ram.dwLength = sizeof ram;
    GlobalMemoryStatusEx(&ram);
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
    std::printf("ChromaStudio §12.2 measurement\n");
    std::printf("  CPU      %s, %d physical cores\n", cpuName().c_str(), physicalCores());
    std::printf("  RAM      %.1f GiB\n", static_cast<double>(ram.ullTotalPhys) / (1ull << 30));
    std::printf("  GPU      %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    std::printf("  driver   %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("  display  %lu Hz, system DPI %u\n", mode.dmDisplayFrequency, GetDpiForSystem());
    std::printf("  scene    %dx%d, %d layers, 25%% tiles (%zu stored), window %dx%d, zoom %s (%.3f px/doc px), %d visible tiles\n",
                kDoc, kDoc, kLayers, tiles.size(), kWindowW, kWindowH, fit ? "fit" : "100%", frame.pixelsPerDocPx, visibleTiles);
    std::printf("  brush    %s, D = %.0f, spacing %.2f; input %.0f Hz, output %.0f Hz; %.0f s after a %.0f-s warm-up\n",
                pen.name.c_str(), pen.tip.diameter, pen.ink.spacing, kInputHz, kOutputHz, seconds, warmup);
    std::printf("  start    GL context %.0f ms, scene in RAM %.0f ms, programs %.0f ms, first document frame %.0f ms "
                "(uploads the visible tiles); first stroke packet → presented %.1f ms\n",
                ms(contextReady - launch), ms(sceneReady - contextReady), ms(programsReady - sceneReady),
                ms(firstFrame - programsReady), firstStroke);
    cpu.print("CPU frame preparation", 4.0);
    gpu.print("GPU frame time", 12.0);
    latency.print("packet → presented frame", 33.0);
    std::printf("  memory   peak working set %.0f MiB; composite cache %zu tiles\n",
                static_cast<double>(memory.PeakWorkingSetSize) / (1 << 20), renderer.cachedTiles());
    timeEndPeriod(1);
    std::fflush(stdout);
    renderer.cancelStroke();
    return 0;  // the renderer and the tiles go before the context (reverse declaration order)
}

#include "PlatformWindow.hpp"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <system_error>
#include <utility>

namespace chroma::platform {
namespace {

constexpr wchar_t kWindowClassName[] = L"ChromaStudio.MainWindow";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kWindowExStyle = 0;

// DWMWA_USE_IMMERSIVE_DARK_MODE, spelled out so older SDK headers still compile.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

// Legacy tablet stack (tpcshrd.h): opt out of press-and-hold, pen tap/barrel feedback and flicks.
constexpr UINT kWmTabletQuerySystemGestureStatus = 0x02CC;
constexpr LRESULT kTabletGestureOptOut = 0x00000001     // TABLET_DISABLE_PRESSANDHOLD
                                         | 0x00000008   // TABLET_DISABLE_PENTAPFEEDBACK
                                         | 0x00000010   // TABLET_DISABLE_PENBARRELFEEDBACK
                                         | 0x00010000;  // TABLET_DISABLE_FLICKS

constexpr double kDegreesToRadians = std::numbers::pi / 180.0;

// ptPixelLocationRaw is the integer sibling of the HIMETRIC position. A mapping that lands further
// away than this is wrong (aspect-locked tablet area, stale rectangles) and is not used.
constexpr double kHimetricTolerancePx = 1.5;

int rectWidth(const RECT& r) noexcept { return r.right - r.left; }
int rectHeight(const RECT& r) noexcept { return r.bottom - r.top; }
int roundToInt(double value) noexcept { return static_cast<int>(std::lround(value)); }

// Mouse messages that Windows synthesised from pen or touch carry MI_WP_SIGNATURE.
bool isPromotedMouseMessage() noexcept {
    const auto extra = static_cast<std::uint32_t>(static_cast<std::uintptr_t>(GetMessageExtraInfo()));
    return (extra & 0xFFFFFF00u) == 0xFF515700u;
}

std::uint32_t mouseButtonsFromKeyState(WPARAM wParam) noexcept {
    const WORD keys = GET_KEYSTATE_WPARAM(wParam);
    std::uint32_t buttons = 0;
    if (keys & MK_LBUTTON) buttons |= Button::Primary;
    if (keys & MK_RBUTTON) buttons |= Button::Secondary;
    if (keys & MK_MBUTTON) buttons |= Button::Middle;
    if (keys & MK_XBUTTON1) buttons |= Button::X1;
    if (keys & MK_XBUTTON2) buttons |= Button::X2;
    return buttons;
}

bool isMouseButtonDown(UINT message) noexcept {
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
           message == WM_XBUTTONDOWN;
}

PointerPhase phaseFromFlags(POINTER_FLAGS flags) noexcept {
    if (flags & POINTER_FLAG_DOWN) return PointerPhase::Down;
    if (flags & POINTER_FLAG_UP) return (flags & POINTER_FLAG_CANCELED) ? PointerPhase::Cancel : PointerPhase::Up;
    if (flags & POINTER_FLAG_INCONTACT) return PointerPhase::Move;
    return PointerPhase::Hover;
}

// Copies the coalesced history of the current pointer message and returns it oldest first. The API
// hands it out newest first (spec §3: copy at once, then reverse).
template <class Info, class GetOne, class GetHistory>
std::vector<Info> copyHistory(UINT32 pointerId, GetOne getOne, GetHistory getHistory) {
    Info current{};
    if (!getOne(pointerId, &current)) return {};
    UINT32 count = std::max<UINT32>(current.pointerInfo.historyCount, 1u);
    std::vector<Info> history(count);
    if (!getHistory(pointerId, &count, history.data()) || count == 0) return {current};
    history.resize(count);
    std::reverse(history.begin(), history.end());
    return history;
}

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────

bool PlatformWindow::enablePerMonitorDpiAwareness() noexcept {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
    // ERROR_ACCESS_DENIED: already set, normally by the manifest. Report what the thread really got.
    return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) !=
           FALSE;
}

PlatformWindow::PlatformWindow(HINSTANCE instance, const WindowDesc& desc, WindowListener& listener)
    : listener_(listener), desc_(desc) {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    qpcFrequency_ = value.QuadPart > 0 ? value.QuadPart : 1;
    QueryPerformanceCounter(&value);
    qpcOrigin_ = value.QuadPart;
    tickOrigin_ = GetTickCount64();

    static const ATOM windowClass = [instance] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;  // CS_OWNDC: the WGL device context lives with the window
        wc.lpfnWndProc = &PlatformWindow::windowProc;
        wc.hInstance = instance;
        // Resource 1 is the app icon (resources/app.ico); the stock icon when it is missing.
        wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        return RegisterClassExW(&wc);
    }();
    if (windowClass == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "RegisterClassExW");
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);

    // Created hidden on the target monitor so that GetDpiForWindow reports that monitor's DPI
    // before the window is sized.
    hwnd_ = CreateWindowExW(kWindowExStyle, kWindowClassName, desc_.title.c_str(), kWindowStyle, monitor.rcWork.left,
                            monitor.rcWork.top, 320, 240, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateWindowExW");
    }
    applyWindowAttributes();
    DragAcceptFiles(hwnd_, TRUE);
    placeInitial(monitor.rcWork);
    refreshMetrics();
}

PlatformWindow::~PlatformWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void PlatformWindow::show(int showCommand) {
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
}

void PlatformWindow::setTitle(std::wstring_view title) {
    if (!hwnd_) return;
    const std::wstring text(title);
    SetWindowTextW(hwnd_, text.c_str());
}

void PlatformWindow::requestClose() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void PlatformWindow::cancelActiveContacts(CancelReason reason) { endAllContacts(PointerPhase::Cancel, reason); }

void PlatformWindow::rethrowPendingFailure() {
    if (failure_) std::rethrow_exception(std::exchange(failure_, nullptr));
}

void PlatformWindow::applyWindowAttributes() {
    if (desc_.darkTitleBar) {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    }
    // No press-and-hold rings, tap ripples or barrel visuals over the canvas: they lag behind the
    // pen and hide the stroke. The editor draws its own cursor.
    constexpr FEEDBACK_TYPE kQuietFeedback[] = {
        FEEDBACK_TOUCH_CONTACTVISUALIZATION, FEEDBACK_PEN_BARRELVISUALIZATION, FEEDBACK_PEN_TAP,
        FEEDBACK_PEN_DOUBLETAP,              FEEDBACK_PEN_PRESSANDHOLD,        FEEDBACK_PEN_RIGHTTAP,
        FEEDBACK_TOUCH_TAP,                  FEEDBACK_TOUCH_DOUBLETAP,         FEEDBACK_TOUCH_PRESSANDHOLD,
        FEEDBACK_TOUCH_RIGHTTAP,             FEEDBACK_GESTURE_PRESSANDTAP,
    };
    const BOOL off = FALSE;
    for (const FEEDBACK_TYPE type : kQuietFeedback) SetWindowFeedbackSetting(hwnd_, type, 0, sizeof(off), &off);

    // WM_POINTERDEVICECHANGE tells us when a digitizer's mapping changes.
    RegisterPointerDeviceNotifications(hwnd_, FALSE);
}

// Spec §11: 1440×900 DIP client, limited by the work area, never below 640×400 DIP; centred.
void PlatformWindow::placeInitial(const RECT& workArea) {
    const UINT dpi = GetDpiForWindow(hwnd_);
    const double scale = dpi / 96.0;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, kWindowExStyle, dpi);
    const int frameWidth = rectWidth(frame);
    const int frameHeight = rectHeight(frame);

    const int minWidth = roundToInt(desc_.minClientWidthDip * scale);
    const int minHeight = roundToInt(desc_.minClientHeightDip * scale);
    const int clientWidth =
        std::max(minWidth, std::min(roundToInt(desc_.clientWidthDip * scale), rectWidth(workArea) - frameWidth));
    const int clientHeight =
        std::max(minHeight, std::min(roundToInt(desc_.clientHeightDip * scale), rectHeight(workArea) - frameHeight));

    const int width = clientWidth + frameWidth;
    const int height = clientHeight + frameHeight;
    const int x = workArea.left + std::max(0, (rectWidth(workArea) - width) / 2);
    const int y = workArea.top + std::max(0, (rectHeight(workArea) - height) / 2);
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ── Geometry ─────────────────────────────────────────────────────────────────

void PlatformWindow::refreshMetrics() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    WindowMetrics next = metrics_;
    const UINT dpi = GetDpiForWindow(hwnd_);
    next.dpi = dpi != 0 ? dpi : 96;
    next.widthPx = rectWidth(client);
    next.heightPx = rectHeight(client);
    next.widthDip = next.widthPx * 96.0 / next.dpi;
    next.heightDip = next.heightPx * 96.0 / next.dpi;
    next.minimized = IsIconic(hwnd_) != FALSE;

    const bool unchanged = metrics_.generation != 0 && next.dpi == metrics_.dpi && next.widthPx == metrics_.widthPx &&
                           next.heightPx == metrics_.heightPx && next.minimized == metrics_.minimized;
    if (unchanged) return;
    next.generation = metrics_.generation + 1;
    metrics_ = next;
    listener_.onMetricsChanged(metrics_);
}

void PlatformWindow::handleDpiChanged(LPARAM lParam) {
    // Spec §2: finish an active stroke with its last confirmed packet before the geometry changes.
    endAllContacts(PointerPhase::Up, CancelReason::None);
    deviceMaps_.clear();

    const RECT suggested = *reinterpret_cast<const RECT*>(lParam);
    inDpiChange_ = true;  // the WM_SIZE sent from SetWindowPos is folded into the generation below
    SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, rectWidth(suggested), rectHeight(suggested),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    inDpiChange_ = false;
    refreshMetrics();  // new DPI and client size published as one generation
}

// Spec §11: when a monitor disappears, bring the window back to an available work area.
void PlatformWindow::keepOnScreen() {
    if (IsIconic(hwnd_) || IsZoomed(hwnd_)) return;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    if (MonitorFromRect(&window, MONITOR_DEFAULTTONULL)) return;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    const int width = std::min(rectWidth(window), rectWidth(work));
    const int height = std::min(rectHeight(window), rectHeight(work));
    SetWindowPos(hwnd_, nullptr, work.left + (rectWidth(work) - width) / 2, work.top + (rectHeight(work) - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

POINT PlatformWindow::clientOrigin() const noexcept {
    POINT origin{0, 0};
    ClientToScreen(hwnd_, &origin);  // signed: monitors left of / above the primary have negative X/Y
    return origin;
}

// ── Time ─────────────────────────────────────────────────────────────────────

double PlatformWindow::qpcSeconds(std::int64_t counter) const noexcept {
    return static_cast<double>(counter - qpcOrigin_) / static_cast<double>(qpcFrequency_);
}

double PlatformWindow::nowSeconds() const noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return qpcSeconds(now.QuadPart);
}

// Spec §3 fallback clock: the 32-bit millisecond stamp, unwrapped against the 64-bit tick count.
double PlatformWindow::tickSeconds(DWORD tickMs) const noexcept {
    const ULONGLONG now = GetTickCount64();
    const DWORD age = static_cast<DWORD>(now) - tickMs;  // modulo 2^32: correct across the 49.7-day wrap
    const ULONGLONG stamp = now - age;
    return (static_cast<double>(stamp) - static_cast<double>(tickOrigin_)) / 1000.0;
}

// ── Window procedure ─────────────────────────────────────────────────────────

LRESULT CALLBACK PlatformWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<PlatformWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    auto* self = reinterpret_cast<PlatformWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);  // WM_GETMINMAXINFO precedes WM_NCCREATE

    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // C++ exceptions must not unwind through user32 frames: park them for the message loop.
    try {
        return self->handleMessage(message, wParam, lParam);
    } catch (...) {
        if (!self->failure_) self->failure_ = std::current_exception();
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

LRESULT PlatformWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    // Pen and touch.
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
        if (handlePointer(wParam)) return 0;
        break;  // other pointer types: DefWindowProc promotes them to mouse messages
    case WM_POINTERLEAVE:
        handlePointerLeave(wParam);
        break;
    case WM_POINTERCAPTURECHANGED:
        handlePointerCaptureChanged(wParam);
        return 0;
    case WM_POINTERDEVICECHANGE:
        deviceMaps_.clear();
        break;
    case kWmTabletQuerySystemGestureStatus:
        return kTabletGestureOptOut;

    // Plain mouse.
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (!isPromotedMouseMessage()) handleMouse(message, wParam, lParam);
        return (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) ? TRUE : 0;
    case WM_MOUSELEAVE:
        mouseLeaveArmed_ = false;
        if (!mouse_.inContact) {
            PointerSample leave = mouse_.last;
            leave.phase = PointerPhase::Leave;
            publishMouse(leave);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (mouse_.inContact) {
            endContact(mouse_, PointerPhase::Cancel, CancelReason::CaptureLost);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // screen coordinates for wheel messages
        ScreenToClient(hwnd_, &at);
        WheelEvent wheel;
        wheel.xPx = at.x + 0.5;
        wheel.yPx = at.y + 0.5;
        wheel.x = wheel.xPx * 96.0 / metrics_.dpi;
        wheel.y = wheel.yPx * 96.0 / metrics_.dpi;
        wheel.notches = GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<double>(WHEEL_DELTA);
        const WORD keys = GET_KEYSTATE_WPARAM(wParam);
        wheel.ctrl = (keys & MK_CONTROL) != 0;
        wheel.shift = (keys & MK_SHIFT) != 0;
        if (listener_.onWheel(wheel)) return 0;
        break;
    }

    // Gesture arbitration (spec §11.2).
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) endAllContacts(PointerPhase::Cancel, CancelReason::Deactivated);
        listener_.onActivationChanged(LOWORD(wParam) != WA_INACTIVE);
        break;
    case WM_CANCELMODE:
        endAllContacts(PointerPhase::Cancel, CancelReason::System);
        break;

    // Keyboard. System keys still reach DefWindowProc unless the editor consumes them.
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        KeyEvent key;
        key.virtualKey = static_cast<std::uint32_t>(wParam);
        key.down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        key.repeat = key.down && (HIWORD(lParam) & KF_REPEAT) != 0;
        key.ctrl = GetKeyState(VK_CONTROL) < 0;
        key.shift = GetKeyState(VK_SHIFT) < 0;
        key.alt = (HIWORD(lParam) & KF_ALTDOWN) != 0;
        if (listener_.onKey(key)) return 0;
        break;
    }
    case WM_CHAR: {
        const auto unit = static_cast<wchar_t>(wParam);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pendingHighSurrogate_ = unit;
            return 0;
        }
        char32_t codePoint = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (pendingHighSurrogate_ == 0) return 0;
            codePoint = 0x10000 + ((static_cast<char32_t>(pendingHighSurrogate_) - 0xD800) << 10) + (unit - 0xDC00);
        }
        pendingHighSurrogate_ = 0;
        listener_.onChar(codePoint);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT at{};
            GetCursorPos(&at);
            ScreenToClient(hwnd_, &at);
            const double scale = 96.0 / metrics_.dpi;
            if (HCURSOR cursor = listener_.cursorAt((at.x + 0.5) * scale, (at.y + 0.5) * scale)) {
                SetCursor(cursor);
                return TRUE;
            }
        }
        break;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
            paths.push_back(std::move(path));
        }
        DragFinish(drop);
        if (!paths.empty()) listener_.onFilesDropped(paths);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        listener_.onSystemSettingsChanged();
        break;

    // Geometry and DPI.
    case WM_DPICHANGED:
        handleDpiChanged(lParam);
        return 0;
    case WM_SIZE:
        if (!inDpiChange_) refreshMetrics();
        return 0;
    case WM_DISPLAYCHANGE:
        deviceMaps_.clear();
        keepOnScreen();
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd_);
        RECT minimum{0, 0, roundToInt(desc_.minClientWidthDip * dpi / 96.0),
                     roundToInt(desc_.minClientHeightDip * dpi / 96.0)};
        AdjustWindowRectExForDpi(&minimum, kWindowStyle, FALSE, kWindowExStyle, dpi);
        info->ptMinTrackSize = {rectWidth(minimum), rectHeight(minimum)};
        return 0;
    }

    // Painting and lifetime.
    case WM_PAINT:
        ValidateRect(hwnd_, nullptr);
        listener_.onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;  // the presenter covers every pixel
    case WM_CLOSE:
        if (listener_.onCloseRequested()) {
            endAllContacts(PointerPhase::Cancel, CancelReason::Closing);
            DestroyWindow(hwnd_);
        }
        return 0;
    case WM_DESTROY:
        listener_.onDestroy();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

// ── Pen and touch ────────────────────────────────────────────────────────────

PlatformWindow::RawAxes PlatformWindow::axesFromPen(const POINTER_PEN_INFO& pen) noexcept {
    RawAxes axes;
    axes.penFlags = pen.penFlags;
    if (pen.penMask & PEN_MASK_PRESSURE) {
        axes.mask |= Axis::Pressure;
        axes.pressure = pen.pressure;
    }
    if (pen.penMask & PEN_MASK_TILT_X) {
        axes.mask |= Axis::TiltX;
        axes.tiltX = pen.tiltX;
    }
    if (pen.penMask & PEN_MASK_TILT_Y) {
        axes.mask |= Axis::TiltY;
        axes.tiltY = pen.tiltY;
    }
    if (pen.penMask & PEN_MASK_ROTATION) {
        axes.mask |= Axis::Twist;
        axes.rotation = pen.rotation;
    }
    return axes;
}

PlatformWindow::RawAxes PlatformWindow::axesFromTouch(const POINTER_TOUCH_INFO& touch) noexcept {
    RawAxes axes;
    if (touch.touchMask & TOUCH_MASK_PRESSURE) {
        axes.mask |= Axis::Pressure;
        axes.pressure = touch.pressure;
    }
    return axes;
}

PlatformWindow::PacketKey PlatformWindow::makeKey(const POINTER_INFO& info, const RawAxes& axes) noexcept {
    PacketKey key;
    key.frameId = info.frameId;
    key.pointerFlags = info.pointerFlags;
    key.penFlags = axes.penFlags;
    key.himetricX = info.ptHimetricLocationRaw.x;
    key.himetricY = info.ptHimetricLocationRaw.y;
    key.pixelX = info.ptPixelLocationRaw.x;
    key.pixelY = info.ptPixelLocationRaw.y;
    key.pressure = axes.pressure;
    key.tiltX = axes.tiltX;
    key.tiltY = axes.tiltY;
    key.rotation = axes.rotation;
    return key;
}

bool PlatformWindow::handlePointer(WPARAM wParam) {
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(pointerId, &type)) return false;
    const POINT origin = clientOrigin();

    if (type == PT_PEN) {
        const auto history = copyHistory<POINTER_PEN_INFO>(pointerId, GetPointerPenInfo, GetPointerPenInfoHistory);
        if (history.empty()) return false;
        for (const POINTER_PEN_INFO& entry : history) {
            processEntry(pointerId, PointerKind::Pen, entry.pointerInfo, axesFromPen(entry), origin);
        }
        return true;
    }
    if (type == PT_TOUCH) {
        const auto history =
            copyHistory<POINTER_TOUCH_INFO>(pointerId, GetPointerTouchInfo, GetPointerTouchInfoHistory);
        if (history.empty()) return false;
        for (const POINTER_TOUCH_INFO& entry : history) {
            processEntry(pointerId, PointerKind::Touch, entry.pointerInfo, axesFromTouch(entry), origin);
        }
        return true;
    }
    return false;  // PT_MOUSE / PT_TOUCHPAD: the plain mouse path handles them
}

void PlatformWindow::processEntry(std::uint32_t pointerId, PointerKind kind, const POINTER_INFO& info,
                                  const RawAxes& axes, POINT clientOrigin) {
    PointerSample sample = makeSample(pointerId, kind, info, axes, clientOrigin);
    Track& track = tracks_[pointerId];

    // De-duplicate (spec §3): drop entries older than the last delivered one, and identical content
    // at the same time. Different positions with one time stamp are all kept.
    const PacketKey key = makeKey(info, axes);
    if (sample.timeSeconds < track.lastTime) return;
    if (sample.timeSeconds == track.lastTime) {
        if (std::find(track.keysAtLastTime.begin(), track.keysAtLastTime.end(), key) != track.keysAtLastTime.end()) {
            return;
        }
    } else {
        track.lastTime = sample.timeSeconds;
        track.keysAtLastTime.clear();
    }
    track.keysAtLastTime.push_back(key);

    const bool eraserNow = (axes.penFlags & (PEN_FLAG_INVERTED | PEN_FLAG_ERASER)) != 0;
    const PointerPhase phase = phaseFromFlags(info.pointerFlags);
    switch (phase) {
    case PointerPhase::Down:
        if (track.inContact) endContact(track, PointerPhase::Cancel, CancelReason::System);  // end never arrived
        track.inContact = true;
        track.swallow = false;
        track.eraser = eraserNow;
        track.lastContactPressure = sample.pressure;
        break;
    case PointerPhase::Move:
        if (!track.inContact) return;  // remainder of a contact we ended, or one that began elsewhere
        break;
    case PointerPhase::Up:
    case PointerPhase::Cancel:
        if (!track.inContact) {
            track.swallow = false;
            if (kind == PointerKind::Touch) tracks_.erase(pointerId);
            return;
        }
        track.inContact = false;
        break;
    case PointerPhase::Hover:
        track.swallow = false;
        if (track.inContact) return;
        break;
    case PointerPhase::Leave:
        break;
    }

    sample.phase = phase;
    if (phase == PointerPhase::Cancel) sample.cancelReason = CancelReason::System;
    if (kind == PointerKind::Pen && (phase == PointerPhase::Hover ? eraserNow : track.eraser)) {
        sample.buttons |= Button::Eraser;  // the eraser end stays the eraser until the pen lifts
    }
    if (phase == PointerPhase::Down || phase == PointerPhase::Move) {
        sample.buttons |= Button::Primary;
        track.lastContactPressure = sample.pressure;
    }
    if (phase == PointerPhase::Up && (sample.validAxes & Axis::Pressure) && axes.pressure == 0) {
        sample.pressure = track.lastContactPressure;  // spec §3: a system zero on up keeps the last contact pressure
    }
    track.last = sample;

    const bool lifted = phase == PointerPhase::Up || phase == PointerPhase::Cancel;
    emit(sample);
    if (kind == PointerKind::Touch && lifted) tracks_.erase(pointerId);
}

PointerSample PlatformWindow::makeSample(std::uint32_t pointerId, PointerKind kind, const POINTER_INFO& info,
                                         const RawAxes& axes, POINT clientOrigin) {
    PointerSample sample;
    sample.pointerId = pointerId;
    sample.kind = kind;
    sample.frameId = info.frameId;
    sample.windowGeneration = metrics_.generation;
    sample.dpi = metrics_.dpi;

    if (info.PerformanceCount != 0) {
        sample.timeSeconds = qpcSeconds(static_cast<std::int64_t>(info.PerformanceCount));
    } else {
        sample.timeSeconds = tickSeconds(info.dwTime);
        sample.flags |= SampleFlag::TickCountTime;
    }

    double screenX = 0.0;
    double screenY = 0.0;
    if (mapHimetric(info.sourceDevice, info.ptHimetricLocationRaw, info.ptPixelLocationRaw, screenX, screenY)) {
        sample.flags |= SampleFlag::HimetricPosition;
    } else {
        screenX = info.ptPixelLocationRaw.x + 0.5;
        screenY = info.ptPixelLocationRaw.y + 0.5;
    }
    sample.xPx = screenX - clientOrigin.x;
    sample.yPx = screenY - clientOrigin.y;
    sample.x = sample.xPx * 96.0 / metrics_.dpi;
    sample.y = sample.yPx * 96.0 / metrics_.dpi;

    sample.validAxes = axes.mask;
    if (axes.mask & Axis::Pressure) {
        sample.pressure = std::clamp(static_cast<float>(axes.pressure) / 1024.0f, 0.0f, 1.0f);
    }
    if (axes.mask & Axis::TiltX) sample.tiltX = static_cast<float>(axes.tiltX * kDegreesToRadians);
    if (axes.mask & Axis::TiltY) sample.tiltY = static_cast<float>(axes.tiltY * kDegreesToRadians);
    if (axes.mask & Axis::Twist) sample.twist = static_cast<float>((axes.rotation % 360u) * kDegreesToRadians);
    if (axes.penFlags & PEN_FLAG_BARREL) sample.buttons |= Button::Barrel;
    return sample;
}

// Sub-pixel position from the digitizer's HIMETRIC grid. The linear map of the device rectangle onto
// its display rectangle is checked against ptPixelLocationRaw; the four quarter-turn orientations are
// tried so that rotated tablets (portrait mode) calibrate themselves on the first packet.
bool PlatformWindow::mapHimetric(HANDLE device, POINT himetric, POINT pixel, double& screenX, double& screenY) {
    auto [it, inserted] = deviceMaps_.try_emplace(device);
    HimetricMap& map = it->second;
    if (inserted) {
        RECT deviceRect{};
        RECT displayRect{};
        map.usable = GetPointerDeviceRects(device, &deviceRect, &displayRect) && rectWidth(deviceRect) > 0 &&
                     rectHeight(deviceRect) > 0 && rectWidth(displayRect) > 0 && rectHeight(displayRect) > 0;
        map.himetric = deviceRect;
        map.screen = displayRect;
    }
    if (!map.usable) return false;

    const double u = (himetric.x - map.himetric.left) / static_cast<double>(rectWidth(map.himetric));
    const double v = (himetric.y - map.himetric.top) / static_cast<double>(rectHeight(map.himetric));
    const auto project = [&](int orientation, double& x, double& y) {
        double s = u;
        double t = v;
        switch (orientation) {
        case 1: s = 1.0 - v; t = u; break;
        case 2: s = 1.0 - u; t = 1.0 - v; break;
        case 3: s = v; t = 1.0 - u; break;
        default: break;
        }
        x = map.screen.left + s * rectWidth(map.screen);
        y = map.screen.top + t * rectHeight(map.screen);
    };
    const auto agrees = [&](double x, double y) {
        return std::abs(x - (pixel.x + 0.5)) <= kHimetricTolerancePx &&
               std::abs(y - (pixel.y + 0.5)) <= kHimetricTolerancePx;
    };

    if (map.orientation >= 0) {
        project(map.orientation, screenX, screenY);
        if (agrees(screenX, screenY)) return true;
    }
    for (int orientation = 0; orientation < 4; ++orientation) {
        if (orientation == map.orientation) continue;
        double x = 0.0;
        double y = 0.0;
        project(orientation, x, y);
        if (agrees(x, y)) {
            map.orientation = orientation;
            screenX = x;
            screenY = y;
            return true;
        }
    }
    return false;
}

void PlatformWindow::handlePointerLeave(WPARAM wParam) {
    const auto it = tracks_.find(GET_POINTERID_WPARAM(wParam));
    if (it == tracks_.end() || it->second.inContact) return;  // a captured contact keeps reporting until Up
    PointerSample leave = it->second.last;
    leave.phase = PointerPhase::Leave;
    leave.cancelReason = CancelReason::None;
    leave.buttons = 0;
    tracks_.erase(it);
    emit(leave);
}

void PlatformWindow::handlePointerCaptureChanged(WPARAM wParam) {
    const auto it = tracks_.find(GET_POINTERID_WPARAM(wParam));
    if (it != tracks_.end() && it->second.inContact) {
        endContact(it->second, PointerPhase::Cancel, CancelReason::CaptureLost);
    }
}

// ── Plain mouse ──────────────────────────────────────────────────────────────

void PlatformWindow::handleMouse(UINT message, WPARAM wParam, LPARAM lParam) {
    PointerSample sample;
    sample.kind = PointerKind::Mouse;
    sample.timeSeconds = nowSeconds();
    sample.xPx = GET_X_LPARAM(lParam) + 0.5;  // signed: under capture the mouse may be left of / above the client
    sample.yPx = GET_Y_LPARAM(lParam) + 0.5;
    sample.x = sample.xPx * 96.0 / metrics_.dpi;
    sample.y = sample.yPx * 96.0 / metrics_.dpi;
    sample.buttons = mouseButtonsFromKeyState(wParam);
    sample.dpi = metrics_.dpi;
    sample.windowGeneration = metrics_.generation;

    if (!mouse_.inContact) {
        if (isMouseButtonDown(message) && !mouse_.swallow) {
            sample.phase = PointerPhase::Down;
            mouse_.inContact = true;
            mouseLeaveArmed_ = false;
            SetCapture(hwnd_);
            publishMouse(sample);
            return;
        }
        if (sample.buttons == 0) mouse_.swallow = false;  // the cancelled press has been released
        if (message != WM_MOUSEMOVE) return;
        sample.phase = PointerPhase::Hover;
        armMouseLeave();
    } else if (sample.buttons == 0) {
        sample.phase = PointerPhase::Up;
        mouse_.inContact = false;  // end flag before ReleaseCapture (spec §11.2)
        publishMouse(sample);
        if (GetCapture() == hwnd_) ReleaseCapture();
        armMouseLeave();  // released outside the client: WM_MOUSELEAVE follows at once
        return;
    } else {
        sample.phase = PointerPhase::Move;  // includes button changes while another button is held
    }

    // WM_MOUSEMOVE also arrives without motion (capture changes, windows moving underneath).
    const PointerSample& last = mouse_.last;
    const bool emittedBefore = mouse_.lastTime != -std::numeric_limits<double>::infinity();
    if (message == WM_MOUSEMOVE && emittedBefore && last.phase != PointerPhase::Leave && last.xPx == sample.xPx &&
        last.yPx == sample.yPx && last.buttons == sample.buttons) {
        return;
    }
    publishMouse(sample);
}

void PlatformWindow::publishMouse(const PointerSample& sample) {
    mouse_.last = sample;
    mouse_.lastTime = sample.timeSeconds;
    emit(sample);
}

void PlatformWindow::armMouseLeave() {
    if (mouseLeaveArmed_) return;
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd_;
    mouseLeaveArmed_ = TrackMouseEvent(&track) != FALSE;
}

// ── Contact lifetime ─────────────────────────────────────────────────────────

void PlatformWindow::endContact(Track& track, PointerPhase phase, CancelReason reason) {
    track.inContact = false;
    track.swallow = true;
    PointerSample sample = track.last;
    sample.phase = phase;
    sample.cancelReason = reason;
    sample.flags |= SampleFlag::Synthetic;
    if (phase == PointerPhase::Up) {
        sample.pressure = track.lastContactPressure;  // the last confirmed packet, same time and place
    } else {
        sample.timeSeconds = nowSeconds();
    }
    if (&track == &mouse_) {
        publishMouse(sample);
    } else {
        track.last = sample;
        emit(sample);
    }
}

void PlatformWindow::endAllContacts(PointerPhase phase, CancelReason reason) {
    std::vector<std::uint32_t> active;
    for (const auto& [id, track] : tracks_) {
        if (track.inContact) active.push_back(id);
    }
    for (const std::uint32_t id : active) {
        const auto it = tracks_.find(id);
        if (it != tracks_.end() && it->second.inContact) endContact(it->second, phase, reason);
    }
    if (mouse_.inContact) {
        endContact(mouse_, phase, reason);  // clears inContact first, so WM_CAPTURECHANGED stays quiet
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
}

void PlatformWindow::emit(PointerSample sample) {
    sample.sequence = ++sequence_;
    listener_.onPointer(sample);
}

}  // namespace chroma::platform

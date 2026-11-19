#include "ui/Theme.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace chroma::ui {
namespace {

Color system(int index, float alpha = 1.0f) {
    const COLORREF c = GetSysColor(index);
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha};
}

}  // namespace

Theme currentTheme() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    const bool on = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                    (contrast.dwFlags & HCF_HIGHCONTRASTON);
    if (!on) return Theme{};

    // Spec §11.2: the system High Contrast scheme is respected.
    Theme t;
    t.highContrast = true;
    const Color window = system(COLOR_WINDOW);
    const Color text = system(COLOR_WINDOWTEXT);
    const Color face = system(COLOR_BTNFACE);
    const Color highlight = system(COLOR_HIGHLIGHT);
    t.window = t.menuBar = t.options = t.toolbar = t.dock = t.statusBar = t.popup = window;
    t.splitter = text;
    t.pasteboard = face;
    t.control = t.controlHover = t.controlPressed = face;
    t.border = text;
    t.text = text;
    t.textDim = text;
    t.textDisabled = system(COLOR_GRAYTEXT);
    t.accent = highlight;
    t.accentText = system(COLOR_HIGHLIGHTTEXT);
    t.selection = highlight;
    t.focus = highlight;
    t.shadow = {0, 0, 0, 0};
    t.danger = highlight;
    return t;
}

}  // namespace chroma::ui

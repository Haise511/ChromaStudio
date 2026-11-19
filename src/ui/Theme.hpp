#pragma once
// ChromaStudio Engine · ui · colours of the chrome (display-referred sRGB, straight alpha).
// The dark theme keeps normal text ≥ 4.5:1 and the focus ring ≥ 3:1 against its background
// (spec §11.2). With Windows High Contrast on, the system colours are used instead.

#include <cstdint>

namespace chroma::ui {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

constexpr Color rgb8(std::uint32_t rgb, float alpha = 1.0f) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

struct Theme {
    bool highContrast = false;
    Color window = rgb8(0x1F1F23);     // behind everything
    Color menuBar = rgb8(0x26262B);
    Color options = rgb8(0x2C2C32);
    Color toolbar = rgb8(0x26262B);
    Color dock = rgb8(0x2A2A30);
    Color statusBar = rgb8(0x1A1A1E);
    Color splitter = rgb8(0x17171A);
    Color pasteboard = rgb8(0x3A3A41);
    Color control = rgb8(0x36363E);    // button / field face
    Color controlHover = rgb8(0x42424B);
    Color controlPressed = rgb8(0x4C4C57);
    Color border = rgb8(0x4A4A54);
    Color text = rgb8(0xE8E8EC);       // 12.9:1 on the menu bar
    Color textDim = rgb8(0xA6A6B0);    // 6.6:1
    Color textDisabled = rgb8(0x6E6E78);
    Color accent = rgb8(0x4C9BFF);     // selection, slider fill; 5.4:1 as the focus ring
    Color accentText = rgb8(0x0B1A2E);
    Color selection = rgb8(0x2F4F7A);
    Color focus = rgb8(0x6AAEFF);
    Color popup = rgb8(0x303037);
    Color shadow = rgb8(0x000000, 0.35f);
    Color danger = rgb8(0xE5484D);
};

/// The dark theme, or the system High Contrast colours when that mode is on.
Theme currentTheme();

}  // namespace chroma::ui

#pragma once
// ChromaStudio Engine · ui · icons drawn from primitives in a 24 × 24 design box, crisp at any DPI.

#include "ui/Layout.hpp"
#include "ui/Theme.hpp"
#include "ui/UiRenderer.hpp"

#include <cstdint>

namespace chroma::ui {

enum class Icon : std::uint8_t {
    // Tools, in toolbar order (spec §11.1).
    Brush, Eraser, Smudge, Blur, Fill, Select, Vector, Transform, Crop, Pipette, Hand,
    // Chrome.
    Eye, EyeOff, Plus, Duplicate, Trash, Folder, Layers, More, ChevronDown, Check, Lock, Clip, Search,
};

/// Draws `icon` centred in `box` (scaled from 24 DIP).
void drawIcon(UiRenderer& r, Icon icon, const Rect& box, Color color);

}  // namespace chroma::ui

#pragma once
// ChromaStudio Engine · desktop · images on the system clipboard (spec §10.1 "Буфер обмена").
//
// Copy puts the image as CF_DIBV5 (32-bit BGRA, straight alpha, sRGB) and as the registered "PNG"
// format; paste prefers PNG and falls back to a 32- or 24-bit DIB. PNG goes through WIC in memory.

#include "core/RasterOps.hpp"

#include <windows.h>

#include <optional>
#include <string>

namespace chroma::desktop {

/// Returns an empty string on success, otherwise the reason in Russian.
std::string copyToClipboard(HWND owner, const core::Image8& image);

/// The clipboard's image, or nullopt with the reason (an empty reason: there is no image).
std::optional<core::Image8> pasteFromClipboard(HWND owner, std::string& error);

}  // namespace chroma::desktop
